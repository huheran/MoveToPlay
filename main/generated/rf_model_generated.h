#pragma once

#include <stdint.h>

#define RF_MODEL_TREE_COUNT 400
#define RF_MODEL_NODE_COUNT 25956
#define RF_MODEL_FEATURE_COUNT 812
#define RF_MODEL_CLASS_COUNT 10

extern const uint16_t rf_model_tree_offsets[RF_MODEL_TREE_COUNT + 1];
extern const int16_t rf_model_children_left[RF_MODEL_NODE_COUNT];
extern const int16_t rf_model_children_right[RF_MODEL_NODE_COUNT];
extern const int16_t rf_model_features[RF_MODEL_NODE_COUNT];
extern const float rf_model_thresholds[RF_MODEL_NODE_COUNT];
extern const uint8_t rf_model_leaf_classes[RF_MODEL_NODE_COUNT];
extern const char *const rf_model_class_names[RF_MODEL_CLASS_COUNT];
