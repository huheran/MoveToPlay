#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "generated/cnn1d_model_generated.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNN_INFER_NODE_COUNT 4
#define CNN_INFER_WINDOW_SIZE CNN1D_WINDOW_SIZE

typedef struct {
    float ax;
    float ay;
    float az;
    float gx;
    float gy;
    float gz;
} cnn_infer_node_sample_t;

typedef struct {
    bool valid;
    uint8_t class_index;
    const char *label;
    float confidence;
    float scores[CNN1D_NUM_CLASSES];
    uint32_t frame_count;
} cnn_infer_result_t;

void cnn_infer_reset(void);
bool cnn_infer_push_frame(const cnn_infer_node_sample_t nodes[CNN_INFER_NODE_COUNT],
                          cnn_infer_result_t *out_result);

#ifdef __cplusplus
}
#endif
