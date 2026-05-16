#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "generated/cnn1d_model_int8_generated.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNN_INT8_INFER_NODE_COUNT 4
#define CNN_INT8_INFER_WINDOW_SIZE CNN1D_INT8_WINDOW_SIZE

typedef struct {
    float ax;
    float ay;
    float az;
    float gx;
    float gy;
    float gz;
} cnn_int8_infer_node_sample_t;

typedef struct {
    bool valid;
    uint8_t class_index;
    const char *label;
    float confidence;
    float scores[CNN1D_INT8_NUM_CLASSES];
    uint32_t frame_count;
} cnn_int8_infer_result_t;

void cnn_int8_infer_reset(void);
bool cnn_int8_infer_push_frame(const cnn_int8_infer_node_sample_t nodes[CNN_INT8_INFER_NODE_COUNT],
                               cnn_int8_infer_result_t *out_result);

#ifdef __cplusplus
}
#endif
