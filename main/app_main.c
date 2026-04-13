#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "imu_lsm6dsv.h"

static const char *TAG = "imu_main";

#define BOARD_NODE_ID                 1
#define IMU_I2C_PORT                  0
#define IMU_I2C_SCL_GPIO              GPIO_NUM_14
#define IMU_I2C_SDA_GPIO              GPIO_NUM_13
#define IMU_I2C_CLK_HZ                400000
#define IMU_CS_GPIO                   GPIO_NUM_12
#define IMU_SA0_GPIO                  GPIO_NUM_11
#define IMU_SA0_LEVEL                 0
#define IMU_SAMPLE_RATE_HZ            100
#define IMU_SAMPLE_PERIOD_MS          (1000 / IMU_SAMPLE_RATE_HZ)
#define IMU_PRINT_DECIMATION          10

#define STATUS_LED_GPIO               GPIO_NUM_38
#define STATUS_LED_ON_LEVEL           1

#define ERROR_BLINK_PAUSE_MS          1500

/* 预留后续按键/串口命令控制，第一版默认开启采样 */
static bool sampling_enabled = true;

static i2c_master_bus_handle_t s_i2c_bus = NULL;

static void led_init(void)
{
    gpio_reset_pin(STATUS_LED_GPIO);
    gpio_set_direction(STATUS_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(STATUS_LED_GPIO, !STATUS_LED_ON_LEVEL);
}

static inline void led_set(bool on)
{
    gpio_set_level(STATUS_LED_GPIO, on ? STATUS_LED_ON_LEVEL : !STATUS_LED_ON_LEVEL);
}

static void led_blink_startup(uint32_t times, uint32_t on_ms, uint32_t off_ms)
{
    for (uint32_t i = 0; i < times; i++) {
        led_set(true);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        led_set(false);
        vTaskDelay(pdMS_TO_TICKS(off_ms));
    }
}

static void imu_ctrl_pins_init(void)
{
    /* LSM6DSV: CS=1 进入 I2C 模式，SA0 决定地址(0x6A/0x6B) */
    gpio_reset_pin(IMU_CS_GPIO);
    gpio_set_direction(IMU_CS_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(IMU_CS_GPIO, 1);

    gpio_reset_pin(IMU_SA0_GPIO);
    gpio_set_direction(IMU_SA0_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(IMU_SA0_GPIO, IMU_SA0_LEVEL);
}

static esp_err_t i2c_master_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = IMU_I2C_PORT,
        .sda_io_num = IMU_I2C_SDA_GPIO,
        .scl_io_num = IMU_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = true,
    };

    return i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
}

static void print_csv_header(void)
{
    printf("# units: accel[g], gyro[dps]\n");
    printf("# sample_rate_hz=%d, print_rate_hz=%d\n", IMU_SAMPLE_RATE_HZ, IMU_SAMPLE_RATE_HZ / IMU_PRINT_DECIMATION);
    printf("ax,ay,az,gx,gy,gz\n");
}

static void imu_error_loop(void)
{
    ESP_LOGE(TAG, "IMU init failed. Check wiring/power and SA0 level.");
    ESP_LOGE(TAG, "Expected I2C addr: 0x%02X or 0x%02X", IMU_LSM6DSV_I2C_ADDRESS_LOW, IMU_LSM6DSV_I2C_ADDRESS_HIGH);

    while (1) {
        /* 快闪三次 + 间隔，便于肉眼识别错误状态 */
        for (int i = 0; i < 3; i++) {
            led_set(true);
            vTaskDelay(pdMS_TO_TICKS(80));
            led_set(false);
            vTaskDelay(pdMS_TO_TICKS(80));
        }
        vTaskDelay(pdMS_TO_TICKS(ERROR_BLINK_PAUSE_MS));
    }
}

static void imu_sampling_task(void *arg)
{
    (void)arg;

    TickType_t last_wake = xTaskGetTickCount();
    uint32_t sample_index = 0;
    uint32_t led_divider = 0;

    while (1) {
        imu_sample_t sample = {0};
        bool data_valid = false;

        if (sampling_enabled) {
            if (imu_lsm6dsv_read_sample(&sample) == ESP_OK) {
                data_valid = true;
            }
        }

        if (data_valid) {

            if ((sample_index % IMU_PRINT_DECIMATION) == 0) {
                printf("%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
                       (double)sample.accel_g[0],
                       (double)sample.accel_g[1],
                       (double)sample.accel_g[2],
                       (double)sample.gyro_dps[0],
                       (double)sample.gyro_dps[1],
                       (double)sample.gyro_dps[2]);
            }
            sample_index++;
        }

        /* 采样阶段周期闪烁：约 5Hz 可见闪烁 */
        led_divider++;
        if (led_divider >= (IMU_SAMPLE_RATE_HZ / 10)) {
            led_divider = 0;
            led_set(true);
        } else if (led_divider == 1) {
            led_set(false);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(IMU_SAMPLE_PERIOD_MS));
    }
}

void app_main(void)
{
    led_init();
    imu_ctrl_pins_init();
    led_blink_startup(3, 120, 120);

    uint8_t imu_addr_in_use = (IMU_SA0_LEVEL == 0) ? IMU_LSM6DSV_I2C_ADDRESS_LOW : IMU_LSM6DSV_I2C_ADDRESS_HIGH;

    ESP_LOGI(TAG, "Booting ESP32-S3 IMU logger");
    ESP_LOGI(TAG, "node_id=%d", BOARD_NODE_ID);
    ESP_LOGI(TAG, "I2C pins: SCL=%d SDA=%d", IMU_I2C_SCL_GPIO, IMU_I2C_SDA_GPIO);
    ESP_LOGI(TAG, "IMU ctrl pins: CS=%d SA0=%d(level=%d)", IMU_CS_GPIO, IMU_SA0_GPIO, IMU_SA0_LEVEL);
    ESP_LOGI(TAG, "IMU addr(pref): 0x%02X", imu_addr_in_use);
    ESP_LOGI(TAG, "sample_rate_hz=%d", IMU_SAMPLE_RATE_HZ);

    if (i2c_master_init() != ESP_OK) {
        ESP_LOGE(TAG, "I2C master init failed");
        imu_error_loop();
    }

    if (imu_lsm6dsv_init(s_i2c_bus, imu_addr_in_use) != ESP_OK) {
        imu_error_loop();
    }
    ESP_LOGI(TAG, "IMU addr(active): 0x%02X", imu_addr_in_use);

    /* IMU 初始化成功后常亮短暂停留，再进入采样闪烁 */
    led_set(true);
    vTaskDelay(pdMS_TO_TICKS(500));

    print_csv_header();

    xTaskCreatePinnedToCore(imu_sampling_task,
                            "imu_sampling_task",
                            4096,
                            NULL,
                            8,
                            NULL,
                            tskNO_AFFINITY);
}
