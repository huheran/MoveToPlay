#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "generated/rf_state_model_generated.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RF_STATE_INFER_NODE_COUNT 4
#define RF_STATE_INFER_WINDOW_SIZE 15

typedef struct {
    float ax;
    float ay;
    float az;
    float gx;
    float gy;
    float gz;
} rf_state_infer_node_sample_t;

typedef struct {
    bool valid;
    uint8_t class_index;
    const char *label;
    float confidence;
    uint16_t votes[RF_STATE_MODEL_CLASS_COUNT];
    uint32_t frame_count;
} rf_state_infer_result_t;

void rf_state_infer_reset(void);
bool rf_state_infer_push_frame(const rf_state_infer_node_sample_t nodes[RF_STATE_INFER_NODE_COUNT],
                               rf_state_infer_result_t *out_result);

#ifdef __cplusplus
}
#endif
