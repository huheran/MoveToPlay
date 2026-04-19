#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "imu_lsm6dsv.h"
#include "m2p_espnow.h"
#include "usb_keyboard.h"

static const char *TAG = "imu_main";

#define MOVE_TO_PLAY_MODE_DONGLE      0
#define MOVE_TO_PLAY_MODE_TRACKER     1

/*
 * User build options.
 *
 * MOVE_TO_PLAY_DEVICE_MODE:
 *   MOVE_TO_PLAY_MODE_DONGLE  = USB dongle. Receive ESP-NOW packets and print them to serial.
 *   MOVE_TO_PLAY_MODE_TRACKER = Wearable tracker. Read IMU samples and send them by ESP-NOW.
 *
 * DONGLE_ENABLE_USB_KEYBOARD:
 *   0 = serial output only. This is the current debug/default dongle behavior.
 *   1 = enable TinyUSB HID keyboard support for later game-control tests.
 */
//#define MOVE_TO_PLAY_DEVICE_MODE      MOVE_TO_PLAY_MODE_TRACKER
#define MOVE_TO_PLAY_DEVICE_MODE      MOVE_TO_PLAY_MODE_DONGLE

#define MOVE_TO_PLAY_ENABLE_ESPNOW        1
#define MOVE_TO_PLAY_ESPNOW_SEND_SAMPLES  1

#define DONGLE_ENABLE_SERIAL_OUTPUT       1
#define DONGLE_ENABLE_USB_KEYBOARD        0
#define DONGLE_ENABLE_USB_KEYBOARD_TEST   0

#define BOARD_NODE_ID                 1 //chest
//#define BOARD_NODE_ID                 2 //right arm

#define IMU_SPI_HOST                  SPI2_HOST
#define IMU_SPI_SCLK_GPIO             GPIO_NUM_12
#define IMU_SPI_MOSI_GPIO             GPIO_NUM_11
#define IMU_SPI_MISO_GPIO             GPIO_NUM_9
#define IMU_SPI_CS_GPIO               GPIO_NUM_10
#define IMU_SPI_CLK_HZ                1000000
#define IMU_SAMPLE_RATE_HZ            100
#define IMU_SAMPLE_PERIOD_MS          (1000 / IMU_SAMPLE_RATE_HZ)
#define IMU_PRINT_DECIMATION          10
#define DONGLE_SERIAL_STATE_RATE_HZ   25
#define DONGLE_SERIAL_STATE_PERIOD_MS (1000 / DONGLE_SERIAL_STATE_RATE_HZ)
#define DONGLE_MAX_TRACKER_NODES      8

#define STATUS_LED_GPIO               GPIO_NUM_38
#define STATUS_LED_ON_LEVEL           1

#define ERROR_BLINK_PAUSE_MS          1500
#define TRACKER_LED_HEARTBEAT_PERIOD_MS 2000
#define TRACKER_LED_HEARTBEAT_ON_MS      120

#define USB_KEYBOARD_TEST_READY_TIMEOUT_MS 15000
#define USB_KEYBOARD_TEST_START_DELAY_MS   8000
#define USB_KEYBOARD_TEST_HOLD_MS          80

#if (MOVE_TO_PLAY_DEVICE_MODE != MOVE_TO_PLAY_MODE_DONGLE) && \
    (MOVE_TO_PLAY_DEVICE_MODE != MOVE_TO_PLAY_MODE_TRACKER)
#error "MOVE_TO_PLAY_DEVICE_MODE must be 0 (dongle) or 1 (tracker)"
#endif

#if DONGLE_ENABLE_USB_KEYBOARD_TEST && !DONGLE_ENABLE_USB_KEYBOARD
#error "DONGLE_ENABLE_USB_KEYBOARD_TEST requires DONGLE_ENABLE_USB_KEYBOARD"
#endif

#if DONGLE_SERIAL_STATE_RATE_HZ <= 0
#error "DONGLE_SERIAL_STATE_RATE_HZ must be greater than 0"
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
    uint32_t led_heartbeat_ms = 0;

    while (1) {
        imu_sample_t sample = {0};
        bool data_valid = false;

        if (sampling_enabled) {
            if (imu_lsm6dsv_read_sample(&sample) == ESP_OK) {
                data_valid = true;
            }
        }

        if (data_valid) {
#if MOVE_TO_PLAY_ENABLE_ESPNOW && MOVE_TO_PLAY_ESPNOW_SEND_SAMPLES
            esp_err_t send_err = m2p_espnow_send_tracker_sample(BOARD_NODE_ID, sample_index, &sample);
            if (send_err != ESP_OK && (sample_index % IMU_SAMPLE_RATE_HZ) == 0) {
                ESP_LOGW(TAG, "ESP-NOW send failed: %s", esp_err_to_name(send_err));
            }
#endif

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

        /* Normal tracker heartbeat: mostly off, one short pulse every few seconds. */
        led_set(led_heartbeat_ms < TRACKER_LED_HEARTBEAT_ON_MS);
        led_heartbeat_ms += IMU_SAMPLE_PERIOD_MS;
        if (led_heartbeat_ms >= TRACKER_LED_HEARTBEAT_PERIOD_MS) {
            led_heartbeat_ms = 0;
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(IMU_SAMPLE_PERIOD_MS));
    }
}

#if MOVE_TO_PLAY_ENABLE_ESPNOW
#if DONGLE_ENABLE_SERIAL_OUTPUT
typedef struct {
    bool valid;
    bool dirty;
    uint8_t src_addr[6];
    int64_t last_rx_us;
    m2p_espnow_tracker_packet_t packet;
} dongle_latest_node_t;

static dongle_latest_node_t s_dongle_latest_nodes[DONGLE_MAX_TRACKER_NODES];

static dongle_latest_node_t *dongle_latest_node_for_id(uint8_t node_id)
{
    if (node_id == 0 || node_id > DONGLE_MAX_TRACKER_NODES) {
        return NULL;
    }

    return &s_dongle_latest_nodes[node_id - 1];
}

static void dongle_store_latest_packet(const m2p_espnow_rx_packet_t *rx_packet)
{
    const uint8_t node_id = rx_packet->packet.node_id;
    dongle_latest_node_t *node = dongle_latest_node_for_id(node_id);
    if (node == NULL) {
        return;
    }

    node->valid = true;
    node->dirty = true;
    node->last_rx_us = esp_timer_get_time();
    memcpy(node->src_addr, rx_packet->src_addr, sizeof(node->src_addr));
    node->packet = rx_packet->packet;
}

static void dongle_print_latest_node(dongle_latest_node_t *node, int64_t now_us)
{
    const m2p_espnow_tracker_packet_t *packet = &node->packet;
    const uint32_t age_ms = (uint32_t)((now_us - node->last_rx_us) / 1000);

    printf("rx,node=%u,seq=%" PRIu32 ",timestamp_us=%" PRIu32
           ",dongle_rx_us=%" PRId64
           ",src=%02X:%02X:%02X:%02X:%02X:%02X,age_ms=%" PRIu32
           ",ax=%.6f,ay=%.6f,az=%.6f,gx=%.6f,gy=%.6f,gz=%.6f\n",
           packet->node_id,
           packet->sequence,
           packet->timestamp_us,
           node->last_rx_us,
           node->src_addr[0],
           node->src_addr[1],
           node->src_addr[2],
           node->src_addr[3],
           node->src_addr[4],
           node->src_addr[5],
           age_ms,
           (double)packet->accel_g[0],
           (double)packet->accel_g[1],
           (double)packet->accel_g[2],
           (double)packet->gyro_dps[0],
           (double)packet->gyro_dps[1],
           (double)packet->gyro_dps[2]);

    node->dirty = false;
}

static void dongle_print_latest_states(void)
{
    const int64_t now_us = esp_timer_get_time();

    for (size_t i = 0; i < DONGLE_MAX_TRACKER_NODES; i++) {
        dongle_latest_node_t *node = &s_dongle_latest_nodes[i];
        if (node->valid && node->dirty) {
            dongle_print_latest_node(node, now_us);
        }
    }
}
#endif

static void espnow_rx_task(void *arg)
{
    (void)arg;

#if DONGLE_ENABLE_SERIAL_OUTPUT
    const TickType_t print_period_ticks = pdMS_TO_TICKS(DONGLE_SERIAL_STATE_PERIOD_MS);
    TickType_t last_print_tick = xTaskGetTickCount();
#endif

    ESP_LOGI(TAG, "dongle waiting for ESP-NOW tracker packets");

    while (1) {
        m2p_espnow_rx_packet_t rx_packet = {0};

#if DONGLE_ENABLE_SERIAL_OUTPUT
        uint32_t wait_ms = 0;
        TickType_t now_tick = xTaskGetTickCount();
        const TickType_t elapsed_ticks = now_tick - last_print_tick;
        if (elapsed_ticks < print_period_ticks) {
            TickType_t remaining_ticks = print_period_ticks - elapsed_ticks;
            wait_ms = (uint32_t)(remaining_ticks * portTICK_PERIOD_MS);
            if (wait_ms == 0) {
                wait_ms = 1;
            }
        }
#else
        const uint32_t wait_ms = 1000;
#endif

        if (m2p_espnow_receive(&rx_packet, wait_ms)) {
#if DONGLE_ENABLE_SERIAL_OUTPUT
            dongle_store_latest_packet(&rx_packet);
#endif
        }

#if DONGLE_ENABLE_SERIAL_OUTPUT
        now_tick = xTaskGetTickCount();
        if ((now_tick - last_print_tick) >= print_period_ticks) {
            dongle_print_latest_states();
            last_print_tick += print_period_ticks;
            if ((now_tick - last_print_tick) >= print_period_ticks) {
                last_print_tick = now_tick;
            }
        }
#endif
    }
}
#endif

#if DONGLE_ENABLE_USB_KEYBOARD && DONGLE_ENABLE_USB_KEYBOARD_TEST
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
#endif

static void start_dongle_mode(void)
{
    ESP_LOGI(TAG, "Starting dongle mode");
    ESP_LOGI(TAG, "role: receive tracker data");
    ESP_LOGI(TAG, "esp-now channel=%d", M2P_ESPNOW_CHANNEL);
    ESP_LOGI(TAG, "serial output=%d", DONGLE_ENABLE_SERIAL_OUTPUT);
#if DONGLE_ENABLE_SERIAL_OUTPUT
    ESP_LOGI(TAG, "serial latest-state rate=%d Hz", DONGLE_SERIAL_STATE_RATE_HZ);
#endif
    ESP_LOGI(TAG, "usb keyboard=%d", DONGLE_ENABLE_USB_KEYBOARD);

    led_set(true);

#if MOVE_TO_PLAY_ENABLE_ESPNOW
    esp_err_t espnow_err = m2p_espnow_init(M2P_ESPNOW_ROLE_DONGLE, M2P_ESPNOW_CHANNEL);
    if (espnow_err != ESP_OK) {
        ESP_LOGW(TAG, "ESP-NOW init failed: %s", esp_err_to_name(espnow_err));
        return;
    }

    xTaskCreatePinnedToCore(espnow_rx_task,
                            "espnow_rx_task",
                            4096,
                            NULL,
                            7,
                            NULL,
                            tskNO_AFFINITY);
#else
    ESP_LOGI(TAG, "ESP-NOW disabled by MOVE_TO_PLAY_ENABLE_ESPNOW");
#endif

#if DONGLE_ENABLE_USB_KEYBOARD
    esp_err_t usb_err = usb_keyboard_init();
    if (usb_err != ESP_OK) {
        ESP_LOGW(TAG, "USB keyboard init failed: %s", esp_err_to_name(usb_err));
        return;
    }

#if DONGLE_ENABLE_USB_KEYBOARD_TEST
    xTaskCreatePinnedToCore(usb_keyboard_test_task,
                            "usb_keyboard_test",
                            3072,
                            NULL,
                            5,
                            NULL,
                            tskNO_AFFINITY);
#endif
#else
    ESP_LOGI(TAG, "USB keyboard disabled for ESP-NOW serial test");
#endif
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
    ESP_LOGI(TAG, "esp-now channel=%d", M2P_ESPNOW_CHANNEL);
    ESP_LOGI(TAG, "esp-now send_samples=%d", MOVE_TO_PLAY_ESPNOW_SEND_SAMPLES);

#if MOVE_TO_PLAY_ENABLE_ESPNOW
    esp_err_t espnow_err = m2p_espnow_init(M2P_ESPNOW_ROLE_TRACKER, M2P_ESPNOW_CHANNEL);
    if (espnow_err != ESP_OK) {
        ESP_LOGW(TAG, "ESP-NOW init failed: %s", esp_err_to_name(espnow_err));
    }
#else
    ESP_LOGI(TAG, "ESP-NOW disabled by MOVE_TO_PLAY_ENABLE_ESPNOW");
#endif

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
