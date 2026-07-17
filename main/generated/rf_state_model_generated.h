#pragma once

#include <stdint.h>

#define RF_STATE_MODEL_TREE_COUNT 50
#define RF_STATE_MODEL_NODE_COUNT 11744
#define RF_STATE_MODEL_FEATURE_COUNT 812
#define RF_STATE_MODEL_CLASS_COUNT 5

extern const uint16_t rf_state_model_tree_offsets[RF_STATE_MODEL_TREE_COUNT + 1];
extern const int16_t rf_state_model_children_left[RF_STATE_MODEL_NODE_COUNT];
extern const int16_t rf_state_model_children_right[RF_STATE_MODEL_NODE_COUNT];
extern const int16_t rf_state_model_features[RF_STATE_MODEL_NODE_COUNT];
extern const float rf_state_model_thresholds[RF_STATE_MODEL_NODE_COUNT];
extern const uint8_t rf_state_model_leaf_classes[RF_STATE_MODEL_NODE_COUNT];
extern const char *const rf_state_model_class_names[RF_STATE_MODEL_CLASS_COUNT];
