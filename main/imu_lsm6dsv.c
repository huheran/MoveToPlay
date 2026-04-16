#include "imu_lsm6dsv.h"

#include <stddef.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "imu_lsm6dsv";

/*
 * LSM6DSV 寄存器映射在控制寄存器上与 LSM6DSO 系列兼容。
 * 若后续使用的子型号寄存器有差异，只需在此处微调寄存器地址和配置值。
 */
#define REG_WHO_AM_I          0x0F
#define REG_CTRL1_XL          0x10
#define REG_CTRL2_G           0x11
#define REG_CTRL3_C           0x12
#define REG_CTRL4_C           0x13
#define REG_CTRL6_C           0x15
#define REG_CTRL7_G           0x16
#define REG_CTRL8_XL          0x17
#define REG_STATUS_REG        0x1E
#define REG_OUTX_L_G          0x22
#define REG_OUTX_L_A          0x28

#define STATUS_GDA_BIT        (1U << 1)

/* 常见 WHO_AM_I：LSM6DSV16X = 0x70。不同料号可在此扩展。 */
#define WHO_AM_I_LSM6DSV      0x70
#define WHO_AM_I_LSM6DSV_ALT  0x71

/* ODR code 0x4, FS_XL=+-4g, FS_G=+-1000dps */
#define CTRL1_XL_104HZ_4G     0x4A
#define CTRL2_G_104HZ_1000DPS 0x48
#define CTRL3_C_BDU_IF_INC    0x44
#define CTRL4_C_DEFAULT        0x00
#define CTRL6_C_1000DPS        0x03
#define CTRL7_G_DEFAULT        0x00
#define CTRL8_XL_4G            0x01

/* 灵敏度：
 * accel FS=+-4g => 0.122 mg/LSB = 0.000122 g/LSB
 * gyro  FS=+-1000dps => 35 mdps/LSB = 0.035 dps/LSB
 */
#define ACCEL_SENS_G_PER_LSB  0.000122f
#define GYRO_SENS_DPS_PER_LSB 0.035f

static spi_device_handle_t s_imu_dev = NULL;

static void imu_detach_device(void)
{
    if (s_imu_dev != NULL) {
        spi_bus_remove_device(s_imu_dev);
        s_imu_dev = NULL;
    }
}

static esp_err_t imu_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(s_imu_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "IMU device not initialized");
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG, "read data is NULL");
    ESP_RETURN_ON_FALSE((len > 0) && (len <= 15), ESP_ERR_INVALID_SIZE, TAG, "read len invalid");

    uint8_t tx_buf[16] = {0};
    uint8_t rx_buf[16] = {0};

    tx_buf[0] = reg | 0x80U;

    spi_transaction_t trans = {
        .length = (len + 1U) * 8U,
        .tx_buffer = tx_buf,
        .rx_buffer = rx_buf,
    };

    esp_err_t err = spi_device_transmit(s_imu_dev, &trans);
    if (err == ESP_OK) {
        memcpy(data, &rx_buf[1], len);
    }
    return err;
}

static esp_err_t imu_write_reg(uint8_t reg, uint8_t value)
{
    ESP_RETURN_ON_FALSE(s_imu_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "IMU device not initialized");

    uint8_t tx_buf[2] = {reg & 0x7FU, value};
    spi_transaction_t trans = {
        .length = sizeof(tx_buf) * 8U,
        .tx_buffer = tx_buf,
    };

    return spi_device_transmit(s_imu_dev, &trans);
}

static int16_t axis_from_le(const uint8_t *buf)
{
    return (int16_t)((buf[1] << 8) | buf[0]);
}

static bool imu_has_gyro_data_ready(uint32_t timeout_ms)
{
    const TickType_t delay_tick = pdMS_TO_TICKS(5);
    const uint32_t loops = (timeout_ms / 5U) + 1U;

    for (uint32_t i = 0; i < loops; i++) {
        uint8_t status = 0;
        uint8_t raw_g[6] = {0};
        if (imu_read_reg(REG_STATUS_REG, &status, 1) != ESP_OK) {
            continue;
        }
        if ((status & STATUS_GDA_BIT) == 0) {
            vTaskDelay(delay_tick);
            continue;
        }

        if (imu_read_reg(REG_OUTX_L_G, raw_g, sizeof(raw_g)) == ESP_OK) {
            const int16_t gx = axis_from_le(&raw_g[0]);
            const int16_t gy = axis_from_le(&raw_g[2]);
            const int16_t gz = axis_from_le(&raw_g[4]);
            if (gx != 0 || gy != 0 || gz != 0) {
                return true;
            }
        }

        vTaskDelay(delay_tick);
    }

    return false;
}

static esp_err_t imu_try_enable_gyro(void)
{
    /*
     * 不同 LSM6DSV/变体在 CTRL2_G 编码细节可能不同。
     * 这里尝试一组保守候选值，以 STATUS_GDA + raw_g 非零作为成功判据。
     */
    static const uint8_t gyro_ctrl2_candidates[] = {
        0x48, /* current assumption: 104Hz + 1000dps */
        0x4C,
        0x44,
        0x84,
        0x40,
        0x04,
    };

    for (size_t i = 0; i < sizeof(gyro_ctrl2_candidates); i++) {
        const uint8_t val = gyro_ctrl2_candidates[i];
        esp_err_t err = imu_write_reg(REG_CTRL2_G, val);
        if (err != ESP_OK) {
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(20));

        uint8_t readback = 0;
        (void)imu_read_reg(REG_CTRL2_G, &readback, 1);
        if (imu_has_gyro_data_ready(60)) {
            ESP_LOGI(TAG, "gyro enabled by CTRL2_G=0x%02X (readback=0x%02X)", val, readback);
            return ESP_OK;
        }

        ESP_LOGW(TAG, "gyro probe miss CTRL2_G=0x%02X (readback=0x%02X)", val, readback);
    }

    return ESP_ERR_NOT_FOUND;
}

static void imu_dump_startup_snapshot(void)
{
    uint8_t who = 0;
    uint8_t ctrl1 = 0;
    uint8_t ctrl2 = 0;
    uint8_t ctrl3 = 0;
    uint8_t ctrl4 = 0;
    uint8_t ctrl6 = 0;
    uint8_t ctrl7 = 0;
    uint8_t ctrl8 = 0;
    uint8_t status = 0;
    uint8_t gyro_raw[6] = {0};
    uint8_t accel_raw[6] = {0};

    (void)imu_read_reg(REG_WHO_AM_I, &who, 1);
    (void)imu_read_reg(REG_CTRL1_XL, &ctrl1, 1);
    (void)imu_read_reg(REG_CTRL2_G, &ctrl2, 1);
    (void)imu_read_reg(REG_CTRL3_C, &ctrl3, 1);
    (void)imu_read_reg(REG_CTRL4_C, &ctrl4, 1);
    (void)imu_read_reg(REG_CTRL6_C, &ctrl6, 1);
    (void)imu_read_reg(REG_CTRL7_G, &ctrl7, 1);
    (void)imu_read_reg(REG_CTRL8_XL, &ctrl8, 1);
    (void)imu_read_reg(REG_STATUS_REG, &status, 1);
    (void)imu_read_reg(REG_OUTX_L_G, gyro_raw, sizeof(gyro_raw));
    (void)imu_read_reg(REG_OUTX_L_A, accel_raw, sizeof(accel_raw));

    const int16_t gx = axis_from_le(&gyro_raw[0]);
    const int16_t gy = axis_from_le(&gyro_raw[2]);
    const int16_t gz = axis_from_le(&gyro_raw[4]);
    const int16_t ax = axis_from_le(&accel_raw[0]);
    const int16_t ay = axis_from_le(&accel_raw[2]);
    const int16_t az = axis_from_le(&accel_raw[4]);

    ESP_LOGI(TAG,
             "snapshot regs: WHO=0x%02X ST=0x%02X CTRL1=0x%02X CTRL2=0x%02X CTRL3=0x%02X CTRL4=0x%02X CTRL6=0x%02X CTRL7=0x%02X CTRL8=0x%02X",
             who,
             status,
             ctrl1,
             ctrl2,
             ctrl3,
             ctrl4,
             ctrl6,
             ctrl7,
             ctrl8);
    ESP_LOGI(TAG,
             "snapshot raw: A[%d,%d,%d] G[%d,%d,%d]",
             ax,
             ay,
             az,
             gx,
             gy,
             gz);

    if (gx == 0 && gy == 0 && gz == 0) {
        ESP_LOGW(TAG, "gyro raw all zero: verify CTRL2_G mapping or exact IMU variant register map");
    }
}

esp_err_t imu_lsm6dsv_init(spi_host_device_t host, int cs_gpio, int clock_hz)
{
    ESP_RETURN_ON_FALSE(clock_hz > 0, ESP_ERR_INVALID_ARG, TAG, "clock_hz invalid");

    imu_detach_device();

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = clock_hz,
        .mode = 3,
        .spics_io_num = cs_gpio,
        .queue_size = 1,
    };

    esp_err_t err = spi_bus_add_device(host, &dev_cfg, &s_imu_dev);
    ESP_RETURN_ON_ERROR(err, TAG, "add spi device failed");

    uint8_t who_am_i = 0;
    err = imu_read_reg(REG_WHO_AM_I, &who_am_i, 1);
    if (err != ESP_OK) {
        imu_detach_device();
        ESP_LOGE(TAG, "read WHO_AM_I failed: %s", esp_err_to_name(err));
        return err;
    }
    if (!(who_am_i == WHO_AM_I_LSM6DSV || who_am_i == WHO_AM_I_LSM6DSV_ALT)) {
        imu_detach_device();
        ESP_LOGE(TAG, "unexpected WHO_AM_I=0x%02X", who_am_i);
        return ESP_ERR_NOT_FOUND;
    }

    /* 推荐初始化顺序：先复位关键控制，再配置 ODR/FS 和滤波 */
    err = imu_write_reg(REG_CTRL3_C, CTRL3_C_BDU_IF_INC);
    if (err != ESP_OK) {
        goto init_fail;
    }
    err = imu_write_reg(REG_CTRL1_XL, CTRL1_XL_104HZ_4G);
    if (err != ESP_OK) {
        goto init_fail;
    }
    err = imu_write_reg(REG_CTRL2_G, CTRL2_G_104HZ_1000DPS);
    if (err != ESP_OK) {
        goto init_fail;
    }
    err = imu_write_reg(REG_CTRL4_C, CTRL4_C_DEFAULT);
    if (err != ESP_OK) {
        goto init_fail;
    }
    err = imu_write_reg(REG_CTRL6_C, CTRL6_C_1000DPS);
    if (err != ESP_OK) {
        goto init_fail;
    }
    err = imu_write_reg(REG_CTRL7_G, CTRL7_G_DEFAULT);
    if (err != ESP_OK) {
        goto init_fail;
    }
    err = imu_write_reg(REG_CTRL8_XL, CTRL8_XL_4G);
    if (err != ESP_OK) {
        goto init_fail;
    }

    /* 若默认配置无法启动 gyro，则进行一次候选探测。 */
    err = imu_try_enable_gyro();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gyro still not ready after CTRL2_G probing");
    }

    /* 给传感器配置稳定时间 */
    vTaskDelay(pdMS_TO_TICKS(20));

    imu_dump_startup_snapshot();
    ESP_LOGI(TAG, "LSM6DSV init ok, WHO_AM_I=0x%02X, spi_host=%d", who_am_i, host);
    return ESP_OK;

init_fail:
    imu_detach_device();
    ESP_LOGE(TAG, "imu init write cfg failed: %s", esp_err_to_name(err));
    return err;
}

esp_err_t imu_lsm6dsv_read_sample(imu_sample_t *out_sample)
{
    ESP_RETURN_ON_FALSE(out_sample != NULL, ESP_ERR_INVALID_ARG, TAG, "out_sample is NULL");

    uint8_t gyro_raw[6] = {0};
    uint8_t accel_raw[6] = {0};

    ESP_RETURN_ON_ERROR(imu_read_reg(REG_OUTX_L_G, gyro_raw, sizeof(gyro_raw)), TAG, "read gyro failed");
    ESP_RETURN_ON_ERROR(imu_read_reg(REG_OUTX_L_A, accel_raw, sizeof(accel_raw)), TAG, "read accel failed");

    const int16_t gx = axis_from_le(&gyro_raw[0]);
    const int16_t gy = axis_from_le(&gyro_raw[2]);
    const int16_t gz = axis_from_le(&gyro_raw[4]);

    const int16_t ax = axis_from_le(&accel_raw[0]);
    const int16_t ay = axis_from_le(&accel_raw[2]);
    const int16_t az = axis_from_le(&accel_raw[4]);

    out_sample->gyro_dps[0] = gx * GYRO_SENS_DPS_PER_LSB;
    out_sample->gyro_dps[1] = gy * GYRO_SENS_DPS_PER_LSB;
    out_sample->gyro_dps[2] = gz * GYRO_SENS_DPS_PER_LSB;

    out_sample->accel_g[0] = ax * ACCEL_SENS_G_PER_LSB;
    out_sample->accel_g[1] = ay * ACCEL_SENS_G_PER_LSB;
    out_sample->accel_g[2] = az * ACCEL_SENS_G_PER_LSB;

    return ESP_OK;
}

