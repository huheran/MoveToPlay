#pragma once

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SA0=0 -> 0x6A, SA0=1 -> 0x6B */
#define IMU_LSM6DSV_I2C_ADDRESS_LOW   0x6A
#define IMU_LSM6DSV_I2C_ADDRESS_HIGH  0x6B
#ifndef IMU_LSM6DSV_I2C_ADDRESS
#define IMU_LSM6DSV_I2C_ADDRESS       IMU_LSM6DSV_I2C_ADDRESS_LOW
#endif

typedef struct {
    float accel_g[3];
    float gyro_dps[3];
} imu_sample_t;

esp_err_t imu_lsm6dsv_init(i2c_master_bus_handle_t bus, uint8_t device_address);
esp_err_t imu_lsm6dsv_read_sample(imu_sample_t *out_sample);

#ifdef __cplusplus
}
#endif
