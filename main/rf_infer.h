#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "generated/rf_model_generated.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RF_INFER_NODE_COUNT 4
#define RF_INFER_WINDOW_SIZE 25

typedef struct {
    float ax;
    float ay;
    float az;
    float gx;
    float gy;
    float gz;
} rf_infer_node_sample_t;

typedef struct {
    bool valid;
    uint8_t class_index;
    const char *label;
    const char *key_text;
    float confidence;
    uint16_t votes[RF_MODEL_CLASS_COUNT];
    uint32_t frame_count;
} rf_infer_result_t;

void rf_infer_reset(void);
bool rf_infer_push_frame(const rf_infer_node_sample_t nodes[RF_INFER_NODE_COUNT],
                         rf_infer_result_t *out_result);

#ifdef __cplusplus
}
#endif
