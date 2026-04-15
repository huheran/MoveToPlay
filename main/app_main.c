#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "imu_lsm6dsv.h"
#include "usb_keyboard.h"

static const char *TAG = "imu_main";

#define MOVE_TO_PLAY_MODE_DONGLE      0
#define MOVE_TO_PLAY_MODE_TRACKER     1

/* 改这里选择固件角色：0 = dongle，1 = tracker。 */
#define MOVE_TO_PLAY_DEVICE_MODE      MOVE_TO_PLAY_MODE_TRACKER

#define BOARD_NODE_ID                 1
#define IMU_SPI_HOST                  SPI2_HOST
#define IMU_SPI_SCLK_GPIO             GPIO_NUM_14
#define IMU_SPI_MOSI_GPIO             GPIO_NUM_13
#define IMU_SPI_MISO_GPIO             GPIO_NUM_11
#define IMU_SPI_CS_GPIO               GPIO_NUM_12
#define IMU_SPI_CLK_HZ                1000000
#define IMU_SAMPLE_RATE_HZ            100
#define IMU_SAMPLE_PERIOD_MS          (1000 / IMU_SAMPLE_RATE_HZ)
#define IMU_PRINT_DECIMATION          10

#define STATUS_LED_GPIO               GPIO_NUM_38
#define STATUS_LED_ON_LEVEL           1

#define ERROR_BLINK_PAUSE_MS          1500

#define USB_KEYBOARD_TEST_READY_TIMEOUT_MS 15000
#define USB_KEYBOARD_TEST_START_DELAY_MS   8000
#define USB_KEYBOARD_TEST_HOLD_MS          80

#if (MOVE_TO_PLAY_DEVICE_MODE != MOVE_TO_PLAY_MODE_DONGLE) && \
    (MOVE_TO_PLAY_DEVICE_MODE != MOVE_TO_PLAY_MODE_TRACKER)
#error "MOVE_TO_PLAY_DEVICE_MODE must be 0 (dongle) or 1 (tracker)"
#endif

/* 预留后续按键/串口命令控制，第一版默认开启采样 */
static bool sampling_enabled = true;

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
    /* SPI 片选空闲为高；后续由 SPI 驱动接管 CS 翻转。 */
    gpio_reset_pin(IMU_SPI_CS_GPIO);
    gpio_set_direction(IMU_SPI_CS_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(IMU_SPI_CS_GPIO, 1);
}

static esp_err_t spi_master_init(void)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = IMU_SPI_MOSI_GPIO,
        .miso_io_num = IMU_SPI_MISO_GPIO,
        .sclk_io_num = IMU_SPI_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16,
    };

    return spi_bus_initialize(IMU_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
}

static void print_csv_header(void)
{
    printf("# units: accel[g], gyro[dps]\n");
    printf("# sample_rate_hz=%d, print_rate_hz=%d\n", IMU_SAMPLE_RATE_HZ, IMU_SAMPLE_RATE_HZ / IMU_PRINT_DECIMATION);
    printf("ax,ay,az,gx,gy,gz\n");
}

static void imu_error_loop(void)
{
    ESP_LOGE(TAG, "IMU init failed. Check wiring/power and SPI pins.");
    ESP_LOGE(TAG, "SPI pins: SCLK=%d MOSI=%d MISO=%d CS=%d",
             IMU_SPI_SCLK_GPIO,
             IMU_SPI_MOSI_GPIO,
             IMU_SPI_MISO_GPIO,
             IMU_SPI_CS_GPIO);

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

static void usb_keyboard_test_task(void *arg)
{
    (void)arg;

    const TickType_t start_tick = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(USB_KEYBOARD_TEST_READY_TIMEOUT_MS);

    while (!usb_keyboard_is_ready()) {
        if ((xTaskGetTickCount() - start_tick) >= timeout_ticks) {
            ESP_LOGW(TAG, "USB keyboard test skipped: device not ready");
            vTaskDelete(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "USB keyboard ready, test key will be sent in %d ms",
             USB_KEYBOARD_TEST_START_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(USB_KEYBOARD_TEST_START_DELAY_MS));

    esp_err_t err = usb_keyboard_tap_key(0, USB_KEYBOARD_KEY_A, USB_KEYBOARD_TEST_HOLD_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "USB keyboard test key failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "USB keyboard test key sent: A");
    }

    vTaskDelete(NULL);
}

static void start_dongle_mode(void)
{
    ESP_LOGI(TAG, "Starting dongle mode");
    ESP_LOGI(TAG, "role: receive tracker data and output USB keyboard");

    led_set(true);

    esp_err_t usb_err = usb_keyboard_init();
    if (usb_err != ESP_OK) {
        ESP_LOGW(TAG, "USB keyboard init failed: %s", esp_err_to_name(usb_err));
        return;
    }

    xTaskCreatePinnedToCore(usb_keyboard_test_task,
                            "usb_keyboard_test",
                            3072,
                            NULL,
                            5,
                            NULL,
                            tskNO_AFFINITY);
}

static void start_tracker_mode(void)
{
    ESP_LOGI(TAG, "Starting tracker mode");
    ESP_LOGI(TAG, "role: read IMU and send tracker data");
    ESP_LOGI(TAG, "node_id=%d", BOARD_NODE_ID);
    ESP_LOGI(TAG, "SPI pins: SCLK=%d MOSI=%d MISO=%d CS=%d",
             IMU_SPI_SCLK_GPIO,
             IMU_SPI_MOSI_GPIO,
             IMU_SPI_MISO_GPIO,
             IMU_SPI_CS_GPIO);
    ESP_LOGI(TAG, "SPI clk_hz=%d", IMU_SPI_CLK_HZ);
    ESP_LOGI(TAG, "sample_rate_hz=%d", IMU_SAMPLE_RATE_HZ);

    imu_ctrl_pins_init();

    if (spi_master_init() != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed");
        imu_error_loop();
    }

    if (imu_lsm6dsv_init(IMU_SPI_HOST, IMU_SPI_CS_GPIO, IMU_SPI_CLK_HZ) != ESP_OK) {
        imu_error_loop();
    }

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

void app_main(void)
{
    led_init();
    led_blink_startup(3, 120, 120);

    ESP_LOGI(TAG, "Booting MoveToPlay");
    ESP_LOGI(TAG, "device_mode=%d (0=dongle, 1=tracker)", MOVE_TO_PLAY_DEVICE_MODE);

    if (MOVE_TO_PLAY_DEVICE_MODE == MOVE_TO_PLAY_MODE_DONGLE) {
        start_dongle_mode();
    } else {
        start_tracker_mode();
    }
}
