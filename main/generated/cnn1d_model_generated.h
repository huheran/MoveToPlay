#pragma once

#include <stdint.h>

#define CNN1D_NUM_CHANNELS 24
#define CNN1D_WINDOW_SIZE 25
#define CNN1D_NUM_CLASSES 10
#define CNN1D_CONV1_OUT 32
#define CNN1D_CONV2_OUT 64
#define CNN1D_CONV3_OUT 64
#define CNN1D_KERNEL_SIZE 3

extern const float cnn1d_norm_mean[CNN1D_NUM_CHANNELS];
extern const float cnn1d_norm_inv_std[CNN1D_NUM_CHANNELS];
extern const float cnn1d_conv1_w[CNN1D_CONV1_OUT * CNN1D_NUM_CHANNELS * CNN1D_KERNEL_SIZE];
extern const float cnn1d_conv1_b[CNN1D_CONV1_OUT];
extern const float cnn1d_conv2_w[CNN1D_CONV2_OUT * CNN1D_CONV1_OUT * CNN1D_KERNEL_SIZE];
extern const float cnn1d_conv2_b[CNN1D_CONV2_OUT];
extern const float cnn1d_conv3_w[CNN1D_CONV3_OUT * CNN1D_CONV2_OUT * CNN1D_KERNEL_SIZE];
extern const float cnn1d_conv3_b[CNN1D_CONV3_OUT];
extern const float cnn1d_fc_w[CNN1D_NUM_CLASSES * CNN1D_CONV3_OUT];
extern const float cnn1d_fc_b[CNN1D_NUM_CLASSES];
extern const char *const cnn1d_class_names[CNN1D_NUM_CLASSES];
