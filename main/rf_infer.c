#include "rf_infer.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define RF_CHANNEL_COUNT 8
#define RF_CH_AX 0
#define RF_CH_AY 1
#define RF_CH_AZ 2
#define RF_CH_GX 3
#define RF_CH_GY 4
#define RF_CH_GZ 5
#define RF_CH_ACC_NORM 6
#define RF_CH_GYRO_NORM 7

static float s_history[RF_INFER_WINDOW_SIZE][RF_INFER_NODE_COUNT][RF_CHANNEL_COUNT];
static uint8_t s_history_head = 0;
static uint8_t s_history_count = 0;
static uint32_t s_total_frames = 0;
static float s_features[RF_MODEL_FEATURE_COUNT];

static const char *const s_class_key_text[RF_MODEL_CLASS_COUNT] = {
    "R",       /* both_hands_raise */
    "F",       /* hands_chest_push */
    "X",       /* hands_cross_chest */
    "-",       /* idle */
    "SPACE",   /* jump */
    "Q",       /* left_hand_raise */
    "E",       /* right_hand_raise */
    "J",       /* right_hand_slash */
    "SHIFT+W", /* run */
    "W",       /* walk */
};

void rf_infer_reset(void)
{
    memset(s_history, 0, sizeof(s_history));
    memset(s_features, 0, sizeof(s_features));
    s_history_head = 0;
    s_history_count = 0;
    s_total_frames = 0;
}

static uint8_t history_physical_index(uint8_t logical_index)
{
    return (uint8_t)((s_history_head + logical_index) % RF_INFER_WINDOW_SIZE);
}

static float history_value(uint8_t logical_index, uint8_t node_index, uint8_t channel)
{
    return s_history[history_physical_index(logical_index)][node_index][channel];
}

static float clamp_nonnegative(float value)
{
    return value > 0.0f ? value : 0.0f;
}

static void append_channel_features(const float values[RF_INFER_WINDOW_SIZE],
                                    float features[RF_MODEL_FEATURE_COUNT],
                                    size_t *feature_index)
{
    float sum = 0.0f;
    float sum_sq = 0.0f;
    float min_v = values[0];
    float max_v = values[0];

    for (size_t i = 0; i < RF_INFER_WINDOW_SIZE; i++) {
        const float v = values[i];
        sum += v;
        sum_sq += v * v;
        if (v < min_v) {
            min_v = v;
        }
        if (v > max_v) {
            max_v = v;
        }
    }

    const float inv_n = 1.0f / (float)RF_INFER_WINDOW_SIZE;
    const float mean = sum * inv_n;
    const float energy = sum_sq * inv_n;
    const float variance = clamp_nonnegative(energy - (mean * mean));

    float jerk_sum = 0.0f;
    float jerk_abs_sum = 0.0f;
    float jerk_sq_sum = 0.0f;
    for (size_t i = 1; i < RF_INFER_WINDOW_SIZE; i++) {
        const float d = values[i] - values[i - 1];
        jerk_sum += d;
        jerk_abs_sum += fabsf(d);
        jerk_sq_sum += d * d;
    }

    const float inv_j = 1.0f / (float)(RF_INFER_WINDOW_SIZE - 1);
    const float jerk_mean = jerk_sum * inv_j;
    const float jerk_energy = jerk_sq_sum * inv_j;
    const float jerk_variance = clamp_nonnegative(jerk_energy - (jerk_mean * jerk_mean));

    features[(*feature_index)++] = mean;
    features[(*feature_index)++] = sqrtf(variance);
    features[(*feature_index)++] = max_v;
    features[(*feature_index)++] = min_v;
    features[(*feature_index)++] = max_v - min_v;
    features[(*feature_index)++] = sqrtf(energy);
    features[(*feature_index)++] = energy;
    features[(*feature_index)++] = jerk_abs_sum * inv_j;
    features[(*feature_index)++] = sqrtf(jerk_variance);
    features[(*feature_index)++] = sqrtf(jerk_energy);
}

static void append_corr_feature(const float x[RF_INFER_WINDOW_SIZE],
                                const float y[RF_INFER_WINDOW_SIZE],
                                float features[RF_MODEL_FEATURE_COUNT],
                                size_t *feature_index)
{
    float sum_x = 0.0f;
    float sum_y = 0.0f;
    float sum_x2 = 0.0f;
    float sum_y2 = 0.0f;
    float sum_xy = 0.0f;

    for (size_t i = 0; i < RF_INFER_WINDOW_SIZE; i++) {
        sum_x += x[i];
        sum_y += y[i];
        sum_x2 += x[i] * x[i];
        sum_y2 += y[i] * y[i];
        sum_xy += x[i] * y[i];
    }

    const float inv_n = 1.0f / (float)RF_INFER_WINDOW_SIZE;
    const float mean_x = sum_x * inv_n;
    const float mean_y = sum_y * inv_n;
    const float var_x = clamp_nonnegative((sum_x2 * inv_n) - (mean_x * mean_x));
    const float var_y = clamp_nonnegative((sum_y2 * inv_n) - (mean_y * mean_y));

    float corr = 0.0f;
    if (var_x >= 1e-9f && var_y >= 1e-9f) {
        const float cov = (sum_xy * inv_n) - (mean_x * mean_y);
        corr = cov / sqrtf(var_x * var_y);
    }

    features[(*feature_index)++] = corr;
}

static bool build_feature_vector(float features[RF_MODEL_FEATURE_COUNT])
{
    float values[RF_INFER_WINDOW_SIZE];
    float values_b[RF_INFER_WINDOW_SIZE];
    size_t feature_index = 0;

    for (uint8_t node = 0; node < RF_INFER_NODE_COUNT; node++) {
        for (uint8_t channel = 0; channel < RF_CHANNEL_COUNT; channel++) {
            for (uint8_t frame = 0; frame < RF_INFER_WINDOW_SIZE; frame++) {
                values[frame] = history_value(frame, node, channel);
            }
            append_channel_features(values, features, &feature_index);
        }
    }

    for (uint8_t node_a = 0; node_a < RF_INFER_NODE_COUNT; node_a++) {
        for (uint8_t node_b = (uint8_t)(node_a + 1); node_b < RF_INFER_NODE_COUNT; node_b++) {
            for (uint8_t channel = 0; channel < RF_CHANNEL_COUNT; channel++) {
                for (uint8_t frame = 0; frame < RF_INFER_WINDOW_SIZE; frame++) {
                    values[frame] = history_value(frame, node_a, channel) -
                                    history_value(frame, node_b, channel);
                }
                append_channel_features(values, features, &feature_index);
            }

            for (uint8_t channel = RF_CH_ACC_NORM; channel <= RF_CH_GYRO_NORM; channel++) {
                for (uint8_t frame = 0; frame < RF_INFER_WINDOW_SIZE; frame++) {
                    values[frame] = history_value(frame, node_a, channel);
                    values_b[frame] = history_value(frame, node_b, channel);
                }
                append_corr_feature(values, values_b, features, &feature_index);
            }
        }
    }

    return feature_index == RF_MODEL_FEATURE_COUNT;
}

static uint8_t rf_model_predict_votes(const float features[RF_MODEL_FEATURE_COUNT],
                                      uint16_t votes[RF_MODEL_CLASS_COUNT])
{
    memset(votes, 0, sizeof(uint16_t) * RF_MODEL_CLASS_COUNT);

    for (uint16_t tree = 0; tree < RF_MODEL_TREE_COUNT; tree++) {
        const uint16_t offset = rf_model_tree_offsets[tree];
        int16_t node = 0;

        while (node >= 0) {
            const uint16_t global = (uint16_t)(offset + (uint16_t)node);
            const int16_t left = rf_model_children_left[global];
            if (left < 0) {
                const uint8_t leaf_class = rf_model_leaf_classes[global];
                if (leaf_class < RF_MODEL_CLASS_COUNT) {
                    votes[leaf_class]++;
                }
                break;
            }

            const int16_t feature_index = rf_model_features[global];
            const float threshold = rf_model_thresholds[global];
            if (feature_index >= 0 &&
                feature_index < RF_MODEL_FEATURE_COUNT &&
                features[feature_index] <= threshold) {
                node = left;
            } else {
                node = rf_model_children_right[global];
            }
        }
    }

    uint8_t best_class = 0;
    uint16_t best_votes = votes[0];
    for (uint8_t class_index = 1; class_index < RF_MODEL_CLASS_COUNT; class_index++) {
        if (votes[class_index] > best_votes) {
            best_votes = votes[class_index];
            best_class = class_index;
        }
    }

    return best_class;
}

bool rf_infer_push_frame(const rf_infer_node_sample_t nodes[RF_INFER_NODE_COUNT],
                         rf_infer_result_t *out_result)
{
    if (nodes == NULL || out_result == NULL) {
        return false;
    }

    for (uint8_t node = 0; node < RF_INFER_NODE_COUNT; node++) {
        const rf_infer_node_sample_t *sample = &nodes[node];
        const float acc_norm = sqrtf((sample->ax * sample->ax) +
                                     (sample->ay * sample->ay) +
                                     (sample->az * sample->az));
        const float gyro_norm = sqrtf((sample->gx * sample->gx) +
                                      (sample->gy * sample->gy) +
                                      (sample->gz * sample->gz));

        s_history[s_history_head][node][RF_CH_AX] = sample->ax;
        s_history[s_history_head][node][RF_CH_AY] = sample->ay;
        s_history[s_history_head][node][RF_CH_AZ] = sample->az;
        s_history[s_history_head][node][RF_CH_GX] = sample->gx;
        s_history[s_history_head][node][RF_CH_GY] = sample->gy;
        s_history[s_history_head][node][RF_CH_GZ] = sample->gz;
        s_history[s_history_head][node][RF_CH_ACC_NORM] = acc_norm;
        s_history[s_history_head][node][RF_CH_GYRO_NORM] = gyro_norm;
    }

    s_history_head = (uint8_t)((s_history_head + 1U) % RF_INFER_WINDOW_SIZE);
    if (s_history_count < RF_INFER_WINDOW_SIZE) {
        s_history_count++;
    }
    s_total_frames++;

    memset(out_result, 0, sizeof(*out_result));
    out_result->frame_count = s_total_frames;

    if (s_history_count < RF_INFER_WINDOW_SIZE) {
        return false;
    }

    if (!build_feature_vector(s_features)) {
        return false;
    }

    const uint8_t best_class = rf_model_predict_votes(s_features, out_result->votes);
    out_result->valid = true;
    out_result->class_index = best_class;
    out_result->label = rf_model_class_names[best_class];
    out_result->key_text = s_class_key_text[best_class];
    out_result->confidence = (float)out_result->votes[best_class] / (float)RF_MODEL_TREE_COUNT;

    return true;
}
