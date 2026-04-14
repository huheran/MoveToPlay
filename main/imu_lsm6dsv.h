#pragma once

#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float accel_g[3];
    float gyro_dps[3];
} imu_sample_t;

esp_err_t imu_lsm6dsv_init(spi_host_device_t host, int cs_gpio, int clock_hz);
esp_err_t imu_lsm6dsv_read_sample(imu_sample_t *out_sample);

#ifdef __cplusplus
}
#endif
