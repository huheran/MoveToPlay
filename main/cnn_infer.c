#include "cnn_infer.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define CNN_CH_PER_NODE 6

static float s_input_buf[CNN1D_NUM_CHANNELS][CNN1D_WINDOW_SIZE];
static uint8_t s_buf_head = 0;
static uint8_t s_buf_count = 0;
static uint32_t s_total_frames = 0;

static float s_conv_out[64][CNN1D_WINDOW_SIZE];
static float s_conv_tmp[64][CNN1D_WINDOW_SIZE];

void cnn_infer_reset(void)
{
    memset(s_input_buf, 0, sizeof(s_input_buf));
    s_buf_head = 0;
    s_buf_count = 0;
    s_total_frames = 0;
}

static void build_normalized_input(float out[CNN1D_NUM_CHANNELS][CNN1D_WINDOW_SIZE])
{
    for (uint8_t t = 0; t < CNN1D_WINDOW_SIZE; t++) {
        uint8_t phys = (uint8_t)((s_buf_head + t) % CNN1D_WINDOW_SIZE);
        for (uint8_t ch = 0; ch < CNN1D_NUM_CHANNELS; ch++) {
            out[ch][t] = (s_input_buf[ch][phys] - cnn1d_norm_mean[ch]) * cnn1d_norm_inv_std[ch];
        }
    }
}

static void conv1d_relu(const float *weights, const float *bias,
                        const float input[][CNN1D_WINDOW_SIZE],
                        float output[][CNN1D_WINDOW_SIZE],
                        int in_channels, int out_channels, int time_len)
{
    const int kernel_size = CNN1D_KERNEL_SIZE;
    const int pad = kernel_size / 2;

    for (int oc = 0; oc < out_channels; oc++) {
        for (int t = 0; t < time_len; t++) {
            float sum = bias[oc];
            for (int ic = 0; ic < in_channels; ic++) {
                for (int k = 0; k < kernel_size; k++) {
                    int ti = t + k - pad;
                    if (ti >= 0 && ti < time_len) {
                        int w_idx = (oc * in_channels + ic) * kernel_size + k;
                        sum += weights[w_idx] * input[ic][ti];
                    }
                }
            }
            output[oc][t] = sum > 0.0f ? sum : 0.0f;
        }
    }
}

static void avg_pool(const float input[][CNN1D_WINDOW_SIZE],
                     float output[], int channels, int time_len)
{
    float inv_t = 1.0f / (float)time_len;
    for (int ch = 0; ch < channels; ch++) {
        float sum = 0.0f;
        for (int t = 0; t < time_len; t++) {
            sum += input[ch][t];
        }
        output[ch] = sum * inv_t;
    }
}

static void linear(const float *weights, const float *bias,
                   const float *input, float *output,
                   int in_features, int out_features)
{
    for (int o = 0; o < out_features; o++) {
        float sum = bias[o];
        for (int i = 0; i < in_features; i++) {
            sum += weights[o * in_features + i] * input[i];
        }
        output[o] = sum;
    }
}

bool cnn_infer_push_frame(const cnn_infer_node_sample_t nodes[CNN_INFER_NODE_COUNT],
                          cnn_infer_result_t *out_result)
{
    if (nodes == NULL || out_result == NULL) {
        return false;
    }

    /* Store frame into circular buffer: 4 nodes × 6 channels = 24 channels */
    for (uint8_t n = 0; n < CNN_INFER_NODE_COUNT; n++) {
        uint8_t base = n * CNN_CH_PER_NODE;
        s_input_buf[base + 0][s_buf_head] = nodes[n].ax;
        s_input_buf[base + 1][s_buf_head] = nodes[n].ay;
        s_input_buf[base + 2][s_buf_head] = nodes[n].az;
        s_input_buf[base + 3][s_buf_head] = nodes[n].gx;
        s_input_buf[base + 4][s_buf_head] = nodes[n].gy;
        s_input_buf[base + 5][s_buf_head] = nodes[n].gz;
    }

    s_buf_head = (uint8_t)((s_buf_head + 1U) % CNN1D_WINDOW_SIZE);
    if (s_buf_count < CNN1D_WINDOW_SIZE) {
        s_buf_count++;
    }
    s_total_frames++;

    memset(out_result, 0, sizeof(*out_result));
    out_result->frame_count = s_total_frames;

    if (s_buf_count < CNN1D_WINDOW_SIZE) {
        return false;
    }

    /* Build normalized input (channels, time) in chronological order */
    float normalized[CNN1D_NUM_CHANNELS][CNN1D_WINDOW_SIZE];
    build_normalized_input(normalized);

    /* Conv1: (24, 25) -> (32, 25) */
    conv1d_relu(cnn1d_conv1_w, cnn1d_conv1_b,
                normalized, s_conv_out,
                CNN1D_NUM_CHANNELS, CNN1D_CONV1_OUT, CNN1D_WINDOW_SIZE);

    /* Conv2: (32, 25) -> (64, 25) */
    conv1d_relu(cnn1d_conv2_w, cnn1d_conv2_b,
                (const float (*)[CNN1D_WINDOW_SIZE])s_conv_out, s_conv_tmp,
                CNN1D_CONV1_OUT, CNN1D_CONV2_OUT, CNN1D_WINDOW_SIZE);

    /* Conv3: (64, 25) -> (64, 25) */
    conv1d_relu(cnn1d_conv3_w, cnn1d_conv3_b,
                (const float (*)[CNN1D_WINDOW_SIZE])s_conv_tmp, s_conv_out,
                CNN1D_CONV2_OUT, CNN1D_CONV3_OUT, CNN1D_WINDOW_SIZE);

    /* AvgPool: (64, 25) -> (64,) */
    float pooled[CNN1D_CONV3_OUT];
    avg_pool((const float (*)[CNN1D_WINDOW_SIZE])s_conv_out,
             pooled, CNN1D_CONV3_OUT, CNN1D_WINDOW_SIZE);

    /* FC: (64,) -> (10,) */
    float logits[CNN1D_NUM_CLASSES];
    linear(cnn1d_fc_w, cnn1d_fc_b, pooled, logits,
           CNN1D_CONV3_OUT, CNN1D_NUM_CLASSES);

    /* Softmax + argmax */
    float max_logit = logits[0];
    uint8_t best_class = 0;
    for (uint8_t i = 1; i < CNN1D_NUM_CLASSES; i++) {
        if (logits[i] > max_logit) {
            max_logit = logits[i];
            best_class = i;
        }
    }

    float sum_exp = 0.0f;
    for (uint8_t i = 0; i < CNN1D_NUM_CLASSES; i++) {
        out_result->scores[i] = expf(logits[i] - max_logit);
        sum_exp += out_result->scores[i];
    }
    float inv_sum = 1.0f / sum_exp;
    for (uint8_t i = 0; i < CNN1D_NUM_CLASSES; i++) {
        out_result->scores[i] *= inv_sum;
    }

    out_result->valid = true;
    out_result->class_index = best_class;
    out_result->label = cnn1d_class_names[best_class];
    out_result->confidence = out_result->scores[best_class];

    return true;
}
