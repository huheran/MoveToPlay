#include "cnn_infer_int8.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define CNN_INT8_CH_PER_NODE 6
#define PADDED_TIME (CNN1D_INT8_WINDOW_SIZE + 24)

static float s_input_buf[CNN1D_INT8_NUM_CHANNELS][CNN1D_INT8_WINDOW_SIZE];
static uint8_t s_buf_head = 0;
static uint8_t s_buf_count = 0;
static uint32_t s_total_frames = 0;

static int8_t s_act_buf_a[64][PADDED_TIME];
static int8_t s_act_buf_b[64][PADDED_TIME];
static float s_float_buf[64][CNN1D_INT8_WINDOW_SIZE];

void cnn_int8_infer_reset(void)
{
    memset(s_input_buf, 0, sizeof(s_input_buf));
    s_buf_head = 0;
    s_buf_count = 0;
    s_total_frames = 0;
}

static inline int8_t clamp_to_int8(int32_t val)
{
    if (val > 127) return 127;
    if (val < -127) return -127;
    return (int8_t)val;
}

static void conv1d_int8_layer(
    const int8_t *weights,
    const float *bias,
    const float *w_scale,
    float input_act_scale,
    float output_act_scale,
    const int8_t *input,
    int8_t *output,
    int in_channels, int out_channels,
    int kernel_size, int dilation, int padding,
    int time_len, int input_stride, int output_stride,
    int output_as_float, float *float_output)
{
    const float inv_out_scale = (output_act_scale > 0.0f) ? (1.0f / output_act_scale) : 0.0f;

    for (int oc = 0; oc < out_channels; oc++) {
        const float combined_scale = w_scale[oc] * input_act_scale;
        const int oc_w_offset = oc * in_channels * kernel_size;

        for (int t = 0; t < time_len; t++) {
            int32_t acc = 0;

            for (int ic = 0; ic < in_channels; ic++) {
                const int8_t *in_row = input + ic * input_stride + padding;
                const int8_t *w_row = weights + oc_w_offset + ic * kernel_size;

                for (int k = 0; k < kernel_size; k++) {
                    acc += (int32_t)w_row[k] * (int32_t)in_row[t + k * dilation];
                }
            }

            float val = (float)acc * combined_scale + bias[oc];
            if (val < 0.0f) val = 0.0f;

            if (output_as_float) {
                float_output[oc * time_len + t] = val;
            } else {
                output[oc * output_stride + t + padding] =
                    clamp_to_int8((int32_t)(val * inv_out_scale + 0.5f));
            }
        }
    }
}

bool cnn_int8_infer_push_frame(const cnn_int8_infer_node_sample_t nodes[CNN_INT8_INFER_NODE_COUNT],
                               cnn_int8_infer_result_t *out_result)
{
    if (nodes == NULL || out_result == NULL) {
        return false;
    }

    for (uint8_t n = 0; n < CNN_INT8_INFER_NODE_COUNT; n++) {
        uint8_t base = n * CNN_INT8_CH_PER_NODE;
        s_input_buf[base + 0][s_buf_head] = nodes[n].ax;
        s_input_buf[base + 1][s_buf_head] = nodes[n].ay;
        s_input_buf[base + 2][s_buf_head] = nodes[n].az;
        s_input_buf[base + 3][s_buf_head] = nodes[n].gx;
        s_input_buf[base + 4][s_buf_head] = nodes[n].gy;
        s_input_buf[base + 5][s_buf_head] = nodes[n].gz;
    }

    s_buf_head = (uint8_t)((s_buf_head + 1U) % CNN1D_INT8_WINDOW_SIZE);
    if (s_buf_count < CNN1D_INT8_WINDOW_SIZE) {
        s_buf_count++;
    }
    s_total_frames++;

    memset(out_result, 0, sizeof(*out_result));
    out_result->frame_count = s_total_frames;

    if (s_buf_count < CNN1D_INT8_WINDOW_SIZE) {
        return false;
    }

    const int T = CNN1D_INT8_WINDOW_SIZE;

    /* Normalize input and quantize to int8, with padding zeros */
    memset(s_act_buf_a, 0, sizeof(s_act_buf_a));
    const float inv_input_scale = 1.0f / cnn1d_int8_input_scale;
    for (uint8_t ch = 0; ch < CNN1D_INT8_NUM_CHANNELS; ch++) {
        for (uint8_t t = 0; t < T; t++) {
            uint8_t phys = (uint8_t)((s_buf_head + t) % T);
            float normalized = (s_input_buf[ch][phys] - cnn1d_int8_norm_mean[ch])
                             * cnn1d_int8_norm_inv_std[ch];
            int32_t q = (int32_t)(normalized * inv_input_scale + (normalized >= 0 ? 0.5f : -0.5f));
            s_act_buf_a[ch][CNN1D_INT8_CONV1_PADDING + t] = clamp_to_int8(q);
        }
    }

    /* Conv1: int8 -> int8 (padded) */
    memset(s_act_buf_b, 0, sizeof(s_act_buf_b));
    conv1d_int8_layer(
        cnn1d_int8_conv1_w, cnn1d_int8_conv1_bias, cnn1d_int8_conv1_w_scale,
        cnn1d_int8_input_scale, cnn1d_int8_conv1_out_scale,
        (const int8_t *)s_act_buf_a, (int8_t *)s_act_buf_b,
        CNN1D_INT8_NUM_CHANNELS, CNN1D_INT8_CONV1_OUT,
        CNN1D_INT8_CONV1_KERNEL, CNN1D_INT8_CONV1_DILATION, CNN1D_INT8_CONV1_PADDING,
        T, PADDED_TIME, PADDED_TIME, 0, NULL);

    /* Conv2: int8 -> int8 (padded) */
    memset(s_act_buf_a, 0, sizeof(s_act_buf_a));
    conv1d_int8_layer(
        cnn1d_int8_conv2_w, cnn1d_int8_conv2_bias, cnn1d_int8_conv2_w_scale,
        cnn1d_int8_conv1_out_scale, cnn1d_int8_conv2_out_scale,
        (const int8_t *)s_act_buf_b, (int8_t *)s_act_buf_a,
        CNN1D_INT8_CONV1_OUT, CNN1D_INT8_CONV2_OUT,
        CNN1D_INT8_CONV2_KERNEL, CNN1D_INT8_CONV2_DILATION, CNN1D_INT8_CONV2_PADDING,
        T, PADDED_TIME, PADDED_TIME, 0, NULL);

    /* Conv3: int8 -> float */
    conv1d_int8_layer(
        cnn1d_int8_conv3_w, cnn1d_int8_conv3_bias, cnn1d_int8_conv3_w_scale,
        cnn1d_int8_conv2_out_scale, 0.0f,
        (const int8_t *)s_act_buf_a, NULL,
        CNN1D_INT8_CONV2_OUT, CNN1D_INT8_CONV3_OUT,
        CNN1D_INT8_CONV3_KERNEL, CNN1D_INT8_CONV3_DILATION, CNN1D_INT8_CONV3_PADDING,
        T, PADDED_TIME, 0, 1, (float *)s_float_buf);

    /* AvgPool: (64, 25) -> (64,) */
    float pooled[CNN1D_INT8_CONV3_OUT];
    const float inv_t = 1.0f / (float)T;
    for (int ch = 0; ch < CNN1D_INT8_CONV3_OUT; ch++) {
        float sum = 0.0f;
        for (int t = 0; t < T; t++) {
            sum += s_float_buf[ch][t];
        }
        pooled[ch] = sum * inv_t;
    }

    /* FC: float */
    float logits[CNN1D_INT8_NUM_CLASSES];
    for (int o = 0; o < CNN1D_INT8_NUM_CLASSES; o++) {
        float sum = cnn1d_int8_fc_b[o];
        for (int i = 0; i < CNN1D_INT8_CONV3_OUT; i++) {
            sum += cnn1d_int8_fc_w[o * CNN1D_INT8_CONV3_OUT + i] * pooled[i];
        }
        logits[o] = sum;
    }

    /* Softmax + argmax */
    float max_logit = logits[0];
    uint8_t best_class = 0;
    for (uint8_t i = 1; i < CNN1D_INT8_NUM_CLASSES; i++) {
        if (logits[i] > max_logit) {
            max_logit = logits[i];
            best_class = i;
        }
    }

    float sum_exp = 0.0f;
    for (uint8_t i = 0; i < CNN1D_INT8_NUM_CLASSES; i++) {
        out_result->scores[i] = expf(logits[i] - max_logit);
        sum_exp += out_result->scores[i];
    }
    float inv_sum = 1.0f / sum_exp;
    for (uint8_t i = 0; i < CNN1D_INT8_NUM_CLASSES; i++) {
        out_result->scores[i] *= inv_sum;
    }

    out_result->valid = true;
    out_result->class_index = best_class;
    out_result->label = cnn1d_int8_class_names[best_class];
    out_result->confidence = out_result->scores[best_class];

    return true;
}
