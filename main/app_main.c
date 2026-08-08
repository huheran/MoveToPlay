#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "imu_lsm6dsv.h"
#include "max30102.h"
#include "m2p_espnow.h"
#include "rf_infer.h"
#include "rf_state_infer.h"
#include "usb_keyboard.h"
#include "battery_monitor.h"
#include "status_led.h"

static const char *TAG = "imu_main";

#define MOVE_TO_PLAY_MODE_DONGLE      0
#define MOVE_TO_PLAY_MODE_TRACKER     1
#define MOVE_TO_PLAY_MODE_BLADE       2

#define DONGLE_MODE_SERIAL_VIEW           0
#define DONGLE_MODE_DATA_COLLECT          1
#define DONGLE_MODE_PLAY                  2

#define TRACKER_NODE_CHEST            1
#define TRACKER_NODE_RIGHT_HAND       2
#define TRACKER_NODE_LEFT_HAND        3
#define TRACKER_NODE_LEG              4

#define M2P_PROFILE_DONGLE            1
#define M2P_PROFILE_BLADE             2
#define M2P_PROFILE_TRACKER_CHEST     3
#define M2P_PROFILE_TRACKER_RIGHT_HAND 4
#define M2P_PROFILE_TRACKER_LEFT_HAND 5
#define M2P_PROFILE_TRACKER_LEG       6



/* 烧录前只改这里。
 * M2P_BOARD_PROFILE: 1=dongle, 2=blade, 3=chest, 4=right_hand, 5=left_hand, 6=leg
 * M2P_DONGLE_MODE:   0=view, 1=collect, 2=play
 * *_BOARD_STYLE:     0=current, 1=new
 */
#ifndef M2P_BOARD_PROFILE
#define M2P_BOARD_PROFILE             1
#endif
#ifndef M2P_DONGLE_MODE
#define M2P_DONGLE_MODE               2
#endif

#define M2P_CHEST_BOARD_STYLE         1
#define M2P_RIGHT_HAND_BOARD_STYLE    1
#define M2P_LEFT_HAND_BOARD_STYLE     1
#define M2P_LEG_BOARD_STYLE           1


#if M2P_BOARD_PROFILE == M2P_PROFILE_DONGLE
#define MOVE_TO_PLAY_DEVICE_MODE      MOVE_TO_PLAY_MODE_DONGLE
#define MOVE_TO_PLAY_TRACKER_NODE_ID  TRACKER_NODE_CHEST
#define MOVE_TO_PLAY_TRACKER_BOARD_STYLE M2P_CHEST_BOARD_STYLE
#elif M2P_BOARD_PROFILE == M2P_PROFILE_BLADE
#define MOVE_TO_PLAY_DEVICE_MODE      MOVE_TO_PLAY_MODE_BLADE
#define MOVE_TO_PLAY_TRACKER_NODE_ID  TRACKER_NODE_CHEST
#define MOVE_TO_PLAY_TRACKER_BOARD_STYLE M2P_CHEST_BOARD_STYLE
#elif M2P_BOARD_PROFILE == M2P_PROFILE_TRACKER_CHEST
#define MOVE_TO_PLAY_DEVICE_MODE      MOVE_TO_PLAY_MODE_TRACKER
#define MOVE_TO_PLAY_TRACKER_NODE_ID  TRACKER_NODE_CHEST
#define MOVE_TO_PLAY_TRACKER_BOARD_STYLE M2P_CHEST_BOARD_STYLE
#elif M2P_BOARD_PROFILE == M2P_PROFILE_TRACKER_RIGHT_HAND
#define MOVE_TO_PLAY_DEVICE_MODE      MOVE_TO_PLAY_MODE_TRACKER
#define MOVE_TO_PLAY_TRACKER_NODE_ID  TRACKER_NODE_RIGHT_HAND
#define MOVE_TO_PLAY_TRACKER_BOARD_STYLE M2P_RIGHT_HAND_BOARD_STYLE
#elif M2P_BOARD_PROFILE == M2P_PROFILE_TRACKER_LEFT_HAND
#define MOVE_TO_PLAY_DEVICE_MODE      MOVE_TO_PLAY_MODE_TRACKER
#define MOVE_TO_PLAY_TRACKER_NODE_ID  TRACKER_NODE_LEFT_HAND
#define MOVE_TO_PLAY_TRACKER_BOARD_STYLE M2P_LEFT_HAND_BOARD_STYLE
#elif M2P_BOARD_PROFILE == M2P_PROFILE_TRACKER_LEG
#define MOVE_TO_PLAY_DEVICE_MODE      MOVE_TO_PLAY_MODE_TRACKER
#define MOVE_TO_PLAY_TRACKER_NODE_ID  TRACKER_NODE_LEG
#define MOVE_TO_PLAY_TRACKER_BOARD_STYLE M2P_LEG_BOARD_STYLE
#else
#error "M2P_BOARD_PROFILE must be 1(dongle), 2(blade), 3(chest), 4(right_hand), 5(left_hand), or 6(leg)"
#endif

#define DONGLE_DATA_COLLECT_MODE      M2P_DONGLE_MODE
#define BOARD_NODE_ID                 MOVE_TO_PLAY_TRACKER_NODE_ID

#define MOVE_TO_PLAY_ENABLE_ESPNOW        1
#define MOVE_TO_PLAY_ESPNOW_SEND_SAMPLES  1

#if (DONGLE_DATA_COLLECT_MODE != DONGLE_MODE_SERIAL_VIEW) && \
    (DONGLE_DATA_COLLECT_MODE != DONGLE_MODE_DATA_COLLECT) && \
    (DONGLE_DATA_COLLECT_MODE != DONGLE_MODE_PLAY)
#error "DONGLE_DATA_COLLECT_MODE must be 0(serial), 1(collect), or 2(play)"
#endif

#if DONGLE_DATA_COLLECT_MODE == DONGLE_MODE_SERIAL_VIEW
#define DONGLE_ENABLE_SERIAL_OUTPUT       1
#define DONGLE_ENABLE_RAW_CSV_OUTPUT      0
#define DONGLE_ENABLE_SERIAL_AGE_COLUMN   1
#define DONGLE_ENABLE_RF_INFERENCE        1
#define DONGLE_USE_CNN_INFER              0
#define DONGLE_ENABLE_HID_ACTION_LOG      1
#define DONGLE_ENABLE_ACTION_DEBUG_OUTPUT 1
#define DONGLE_ENABLE_HID_EVENT_LOG       1
#define DONGLE_ENABLE_USB_KEYBOARD        0
#define DONGLE_ENABLE_USB_MOUSE           0
#define DONGLE_ENABLE_USB_TELEMETRY        0
#define DONGLE_ENABLE_USB_KEYBOARD_TEST   0
#define DONGLE_ENABLE_USB_MOUSE_TEST      0
#elif DONGLE_DATA_COLLECT_MODE == DONGLE_MODE_DATA_COLLECT
#define DONGLE_ENABLE_SERIAL_OUTPUT       1
#define DONGLE_ENABLE_RAW_CSV_OUTPUT      1
#define DONGLE_ENABLE_SERIAL_AGE_COLUMN   1
#define DONGLE_ENABLE_RF_INFERENCE        0
#define DONGLE_USE_CNN_INFER              0
#define DONGLE_ENABLE_HID_ACTION_LOG      0
#define DONGLE_ENABLE_ACTION_DEBUG_OUTPUT 0
#define DONGLE_ENABLE_HID_EVENT_LOG       0
#define DONGLE_ENABLE_USB_KEYBOARD        0
#define DONGLE_ENABLE_USB_MOUSE           0
#define DONGLE_ENABLE_USB_TELEMETRY        0
#define DONGLE_ENABLE_USB_KEYBOARD_TEST   0
#define DONGLE_ENABLE_USB_MOUSE_TEST      0
#else
#define DONGLE_ENABLE_SERIAL_OUTPUT       1
#define DONGLE_ENABLE_RAW_CSV_OUTPUT      0
#define DONGLE_ENABLE_SERIAL_AGE_COLUMN   1
#define DONGLE_ENABLE_RF_INFERENCE        1
#define DONGLE_USE_CNN_INFER              0
#define DONGLE_ENABLE_HID_ACTION_LOG      1
#define DONGLE_ENABLE_ACTION_DEBUG_OUTPUT 0
#define DONGLE_ENABLE_HID_EVENT_LOG       0
#define DONGLE_ENABLE_USB_KEYBOARD        1
#define DONGLE_ENABLE_USB_MOUSE           1
#define DONGLE_ENABLE_USB_TELEMETRY        1
#define DONGLE_ENABLE_USB_KEYBOARD_TEST   0
#define DONGLE_ENABLE_USB_MOUSE_TEST      0
#endif

#include "move_to_play_board_config.h"

#define IMU_SPI_HOST                  SPI2_HOST
#define IMU_SPI_CLK_HZ                1000000
#define IMU_SAMPLE_RATE_HZ            100
#define IMU_SAMPLE_PERIOD_MS          (1000 / IMU_SAMPLE_RATE_HZ)
#define IMU_PRINT_DECIMATION          10
#define DONGLE_SERIAL_STATE_RATE_HZ   25
#define DONGLE_SERIAL_STATE_PERIOD_MS (1000 / DONGLE_SERIAL_STATE_RATE_HZ)
#define DONGLE_TELEMETRY_PERIOD_MS    100
#define DONGLE_TELEMETRY_STALE_MS     750
#define DONGLE_MAX_TRACKER_NODES      8
#define DONGLE_WIFI_DISPLAY_GPIO      GPIO_NUM_4
#define DONGLE_WIFI_DISPLAY_ACTIVE_LEVEL 0
#define DONGLE_WIFI_DISPLAY_HOLD_MS   4000
#define DONGLE_WIFI_DISPLAY_POLL_MS   50
#define DONGLE_WIFI_DISPLAY_AP_SSID   "MoveToPlay-Dongle"
#define DONGLE_WIFI_DISPLAY_AP_PASS   ""
#define DONGLE_WIFI_DISPLAY_AP_MAX_CONN 2
#define DONGLE_WIFI_DISPLAY_STATUS_UPDATE_MS 200
#define DONGLE_WIFI_DISPLAY_RX_DRAIN_DELAY_MS 5
#define DONGLE_STATUS_ONLINE_MAX_AGE_MS 1500
#define DONGLE_RF_MAX_NODE_AGE_MS     250
#define DONGLE_RF_PRINT_INTERVAL_MS   120
#define DONGLE_RF_MIN_CONFIDENCE      0.60f
#define DONGLE_BLADE_MAX_AGE_MS       300
#define DONGLE_BLADE_TURN_PERIOD_MS   20
#define DONGLE_BLADE_MOVE_RELEASE_GRACE_MS 220
#define DONGLE_BLADE_TURN_CHEST_NODE_ID TRACKER_NODE_CHEST
/* 0=gx, 1=gy, 2=gz. Current turn data shows chest gy has the strongest left/right yaw signal. */
#define DONGLE_BLADE_TURN_GYRO_AXIS   1
/* Mouse X is positive for turning right. With the current chest mounting, gy positive means left. */
#define DONGLE_BLADE_TURN_GYRO_SIGN   -1.0f
#define DONGLE_BLADE_TURN_DEADZONE_DPS 8.0f
#define DONGLE_BLADE_TURN_SENSITIVITY 10.0f /* mouse px per integrated gyro degree */
#define DONGLE_BLADE_TURN_MAX_STEP_DELTA 80
/* Chest gx is the shoulder-axis rotation used for looking up/down while Blade is held. */
#define DONGLE_BLADE_PITCH_GYRO_AXIS  0
/* USB mouse Y is positive downward. Flip this sign if the mounted chest tracker is inverted. */
#define DONGLE_BLADE_PITCH_GYRO_SIGN  1.0f
#define DONGLE_BLADE_PITCH_DEADZONE_DPS 8.0f
#define DONGLE_BLADE_PITCH_SENSITIVITY 8.0f /* lower than horizontal to reduce vertical jitter */
#define DONGLE_BLADE_PITCH_MAX_STEP_DELTA 64
#define DONGLE_BLADE_TURN_CHEST_MAX_AGE_MS 150

#define BLADE_NODE_ID                 100
#define BLADE_BUTTON_GPIO             GPIO_NUM_4
#define BLADE_POLL_RATE_HZ            50
#define BLADE_POLL_PERIOD_MS          (1000 / BLADE_POLL_RATE_HZ)
#define BLADE_IDLE_HEARTBEAT_RATE_HZ  5
#define BLADE_IDLE_HEARTBEAT_PERIOD_MS (1000 / BLADE_IDLE_HEARTBEAT_RATE_HZ)
#define BLADE_PRESSED_REPORT_RATE_HZ  25
#define BLADE_PRESSED_REPORT_PERIOD_MS (1000 / BLADE_PRESSED_REPORT_RATE_HZ)
#define BLADE_STATE_CHANGE_BURST_COUNT 3
#define BLADE_DEBOUNCE_SAMPLES        3
#define BLADE_ENABLE_SERIAL_OUTPUT    0
#define BLADE_SERIAL_STATE_RATE_HZ    10
#define BLADE_SERIAL_STATE_PERIOD_MS  (1000 / BLADE_SERIAL_STATE_RATE_HZ)
#define BLADE_SLEEP_SHORT_PRESS_MAX_MS 600
#define BLADE_SLEEP_SHORT_PRESS_COUNT  4
#define BLADE_SLEEP_SEQUENCE_GAP_MS    700
#define BLADE_SLEEP_HOLD_MS           5000
#define BLADE_WAKE_CONFIRM_HOLD_MS    3000

/* Heart-rate connector on the Blade: ESP32 side of the PCA9306 is 3.3 V. */
#define BLADE_HEART_RATE_SDA_GPIO     GPIO_NUM_6
#define BLADE_HEART_RATE_SCL_GPIO     GPIO_NUM_5
#define BLADE_HEART_RATE_INT_GPIO     GPIO_NUM_7
#define BLADE_HEART_RATE_POLL_MS      20
#define BLADE_HEART_RATE_SERIAL_MS    1000
#define BLADE_HEART_RATE_FINGER_MIN_IR 10000U

#define BATTERY_REPORT_INTERVAL_MS    5000

#define ERROR_BLINK_PAUSE_MS          1500
#define TRACKER_LED_HEARTBEAT_PERIOD_MS 2000
#define TRACKER_LED_HEARTBEAT_ON_MS      120

#define USB_KEYBOARD_TEST_READY_TIMEOUT_MS 15000
#define USB_KEYBOARD_TEST_START_DELAY_MS   8000
#define USB_KEYBOARD_TEST_HOLD_MS          80
#define USB_MOUSE_TEST_START_OFFSET_MS     250
#define USB_MOUSE_TEST_LEFT_DELTA          80
#define USB_MOUSE_TEST_RIGHT_DELTA         48
#define USB_MOUSE_TEST_UP_DELTA            48
#define USB_MOUSE_TEST_DOWN_DELTA          48
#define USB_MOUSE_TEST_STEP_MS            120

#if (MOVE_TO_PLAY_DEVICE_MODE != MOVE_TO_PLAY_MODE_DONGLE) && \
    (MOVE_TO_PLAY_DEVICE_MODE != MOVE_TO_PLAY_MODE_TRACKER) && \
    (MOVE_TO_PLAY_DEVICE_MODE != MOVE_TO_PLAY_MODE_BLADE)
#error "MOVE_TO_PLAY_DEVICE_MODE must be 0 (dongle), 1 (tracker), or 2 (blade)"
#endif

#if DONGLE_ENABLE_USB_KEYBOARD_TEST && !DONGLE_ENABLE_USB_KEYBOARD
#error "DONGLE_ENABLE_USB_KEYBOARD_TEST requires DONGLE_ENABLE_USB_KEYBOARD"
#endif

#if DONGLE_ENABLE_USB_MOUSE_TEST && !DONGLE_ENABLE_USB_MOUSE
#error "DONGLE_ENABLE_USB_MOUSE_TEST requires DONGLE_ENABLE_USB_MOUSE"
#endif

#if DONGLE_SERIAL_STATE_RATE_HZ <= 0
#error "DONGLE_SERIAL_STATE_RATE_HZ must be greater than 0"
#endif

#if BLADE_ENABLE_SERIAL_OUTPUT && BLADE_SERIAL_STATE_RATE_HZ <= 0
#error "BLADE_SERIAL_STATE_RATE_HZ must be greater than 0"
#endif

#if BLADE_POLL_RATE_HZ <= 0
#error "BLADE_POLL_RATE_HZ must be greater than 0"
#endif

#if BLADE_IDLE_HEARTBEAT_RATE_HZ <= 0
#error "BLADE_IDLE_HEARTBEAT_RATE_HZ must be greater than 0"
#endif

#if BLADE_PRESSED_REPORT_RATE_HZ <= 0
#error "BLADE_PRESSED_REPORT_RATE_HZ must be greater than 0"
#endif

#if BLADE_STATE_CHANGE_BURST_COUNT <= 0
#error "BLADE_STATE_CHANGE_BURST_COUNT must be greater than 0"
#endif

#if (MOVE_TO_PLAY_TRACKER_NODE_ID < 1) || (MOVE_TO_PLAY_TRACKER_NODE_ID > DONGLE_MAX_TRACKER_NODES)
#error "MOVE_TO_PLAY_TRACKER_NODE_ID must be in range 1..DONGLE_MAX_TRACKER_NODES"
#endif

/* 预留后续按键/串口命令控制，第一版默认开启采样 */
static bool sampling_enabled = true;
static volatile bool s_local_battery_valid = false;
static volatile uint8_t s_local_battery_percent = M2P_ESPNOW_BATTERY_PERCENT_UNKNOWN;
static volatile uint16_t s_local_battery_mv = 0;

static uint16_t battery_voltage_to_mv(float voltage)
{
    if (voltage <= 0.0f) {
        return 0;
    }

    const float mv = voltage * 1000.0f;
    if (mv >= 65535.0f) {
        return 65535U;
    }

    return (uint16_t)(mv + 0.5f);
}

static const char *tracker_node_name(uint8_t node_id)
{
    switch (node_id) {
    case TRACKER_NODE_CHEST:
        return "chest";
    case TRACKER_NODE_RIGHT_HAND:
        return "right_hand";
    case TRACKER_NODE_LEFT_HAND:
        return "left_hand";
    case TRACKER_NODE_LEG:
        return "leg";
    default:
        return "custom";
    }
}

static void led_init(void)
{
    status_led_init(STATUS_LED_SK6812_GPIO);
}

static inline void led_set(bool on)
{
    if (on) {
        status_led_set_color(10, 10, 10);
    } else {
        status_led_off();
    }
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
            esp_err_t send_err = m2p_espnow_send_tracker_sample(BOARD_NODE_ID,
                                                                sample_index,
                                                                &sample,
                                                                s_local_battery_valid,
                                                                s_local_battery_percent,
                                                                s_local_battery_mv);
            if (send_err != ESP_OK && (sample_index % IMU_SAMPLE_RATE_HZ) == 0) {
                ESP_LOGW(TAG, "ESP-NOW send failed: %s", esp_err_to_name(send_err));
            }
#endif

            if ((sample_index % IMU_PRINT_DECIMATION) == 0) {
                /* Tracker serial output: battery only (via battery_monitor_task) */
            }
            sample_index++;
        }

        /* Tracker heartbeat: briefly turn off LED to indicate alive, battery color is set by battery_monitor_task */
        if (led_heartbeat_ms < TRACKER_LED_HEARTBEAT_ON_MS) {
            status_led_off();
        }
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
    bool battery_valid;
    uint8_t src_addr[6];
    int64_t last_rx_us;
    int64_t last_battery_rx_us;
    uint16_t battery_mv;
    uint8_t battery_percent;
    m2p_espnow_tracker_packet_t packet;
} dongle_latest_node_t;

typedef struct {
    bool valid;
    bool pressed;
    bool dirty;
    bool battery_valid;
    uint8_t src_addr[6];
    uint8_t node_id;
    uint32_t sequence;
    uint32_t timestamp_us;
    int64_t last_rx_us;
    int64_t last_battery_rx_us;
    uint16_t battery_mv;
    uint8_t battery_percent;
} dongle_blade_state_t;

static dongle_latest_node_t s_dongle_latest_nodes[DONGLE_MAX_TRACKER_NODES];
static dongle_blade_state_t s_dongle_blade_state;
static SemaphoreHandle_t s_dongle_state_mutex;
#if DONGLE_ENABLE_USB_KEYBOARD || DONGLE_ENABLE_USB_MOUSE
static SemaphoreHandle_t s_dongle_hid_mutex;
#endif
static int64_t s_dongle_display_last_tracker_store_us[DONGLE_MAX_TRACKER_NODES];
#if DONGLE_ENABLE_USB_MOUSE
static int64_t s_dongle_last_blade_turn_us = 0;
static float s_dongle_blade_turn_dx_remainder = 0.0f;
static float s_dongle_blade_turn_dy_remainder = 0.0f;
#endif
#if DONGLE_ENABLE_ACTION_DEBUG_OUTPUT
static int64_t s_dongle_last_rf_print_us = 0;
#endif

static dongle_latest_node_t *dongle_latest_node_for_id(uint8_t node_id)
{
    if (node_id == 0 || node_id > DONGLE_MAX_TRACKER_NODES) {
        return NULL;
    }

    return &s_dongle_latest_nodes[node_id - 1];
}

static void dongle_state_lock(void)
{
    if (s_dongle_state_mutex != NULL) {
        (void)xSemaphoreTake(s_dongle_state_mutex, portMAX_DELAY);
    }
}

static void dongle_state_unlock(void)
{
    if (s_dongle_state_mutex != NULL) {
        (void)xSemaphoreGive(s_dongle_state_mutex);
    }
}

static void dongle_store_latest_packet(const m2p_espnow_rx_packet_t *rx_packet)
{
    if (rx_packet->packet.type != M2P_ESPNOW_PACKET_TRACKER_IMU) {
        return;
    }

    const uint8_t node_id = rx_packet->packet.node_id;
    dongle_latest_node_t *node = dongle_latest_node_for_id(node_id);
    if (node == NULL) {
        return;
    }

    dongle_state_lock();

    if (!node->valid) {
        ESP_LOGI(TAG,
                 "tracker online: node_id=%u (%s)",
                 node_id,
                 tracker_node_name(node_id));
    }

    node->valid = true;
    node->dirty = true;
    node->last_rx_us = esp_timer_get_time();
    memcpy(node->src_addr, rx_packet->src_addr, sizeof(node->src_addr));
    node->packet = rx_packet->packet;
    if ((rx_packet->packet.flags & M2P_ESPNOW_TRACKER_FLAG_BATTERY_VALID) != 0 &&
        rx_packet->packet.battery_percent <= 100U) {
        node->battery_valid = true;
        node->battery_percent = rx_packet->packet.battery_percent;
        node->battery_mv = rx_packet->packet.battery_mv;
        node->last_battery_rx_us = node->last_rx_us;
    }

    dongle_state_unlock();
}

static bool dongle_wifi_display_should_store_tracker_packet(const m2p_espnow_rx_packet_t *rx_packet,
                                                            int64_t now_us)
{
    const uint8_t node_id = rx_packet->packet.node_id;
    if (node_id == 0 || node_id > DONGLE_MAX_TRACKER_NODES) {
        return false;
    }

    int64_t *last_store_us = &s_dongle_display_last_tracker_store_us[node_id - 1U];
    if (*last_store_us <= 0 ||
        (now_us - *last_store_us) >= ((int64_t)DONGLE_WIFI_DISPLAY_STATUS_UPDATE_MS * 1000LL)) {
        *last_store_us = now_us;
        return true;
    }

    return false;
}

static void dongle_store_blade_packet(const m2p_espnow_rx_packet_t *rx_packet)
{
    if (rx_packet->packet.type != M2P_ESPNOW_PACKET_BLADE_STATE) {
        return;
    }

    const bool pressed =
        (rx_packet->packet.flags & M2P_ESPNOW_BLADE_FLAG_PRESSED) != 0;

    dongle_state_lock();

    const bool changed = !s_dongle_blade_state.valid ||
                         s_dongle_blade_state.pressed != pressed;

    if (!s_dongle_blade_state.valid) {
        ESP_LOGI(TAG,
                 "blade online: node_id=%u",
                 rx_packet->packet.node_id);
    }

    s_dongle_blade_state.valid = true;
    s_dongle_blade_state.pressed = pressed;
    s_dongle_blade_state.dirty = s_dongle_blade_state.dirty || changed;
    s_dongle_blade_state.node_id = rx_packet->packet.node_id;
    s_dongle_blade_state.sequence = rx_packet->packet.sequence;
    s_dongle_blade_state.timestamp_us = rx_packet->packet.timestamp_us;
    s_dongle_blade_state.last_rx_us = esp_timer_get_time();
    if ((rx_packet->packet.flags & M2P_ESPNOW_BLADE_FLAG_BATTERY_VALID) != 0 &&
        rx_packet->packet.battery_percent <= 100U) {
        s_dongle_blade_state.battery_valid = true;
        s_dongle_blade_state.battery_percent = rx_packet->packet.battery_percent;
        s_dongle_blade_state.battery_mv = rx_packet->packet.battery_mv;
        s_dongle_blade_state.last_battery_rx_us = s_dongle_blade_state.last_rx_us;
    }
    memcpy(s_dongle_blade_state.src_addr,
           rx_packet->src_addr,
           sizeof(s_dongle_blade_state.src_addr));

    dongle_state_unlock();
}

static double dongle_blade_age_ms(int64_t now_us)
{
    if (!s_dongle_blade_state.valid || s_dongle_blade_state.last_rx_us <= 0) {
        return -1.0;
    }

    const int64_t age_us = now_us - s_dongle_blade_state.last_rx_us;
    if (age_us < 0) {
        return -1.0;
    }

    return (double)age_us / 1000.0;
}

static bool dongle_blade_state_fresh(int64_t now_us)
{
    const double age_ms = dongle_blade_age_ms(now_us);
    return age_ms >= 0.0 && age_ms <= (double)DONGLE_BLADE_MAX_AGE_MS;
}

static void dongle_print_blade_state_if_changed(int64_t now_us)
{
    if (!s_dongle_blade_state.valid || !s_dongle_blade_state.dirty) {
        return;
    }

    const double age_ms = dongle_blade_age_ms(now_us);

    printf("# blade: node_id=%u pressed=%d seq=%" PRIu32 " age_ms=%.1f\n",
           s_dongle_blade_state.node_id,
           s_dongle_blade_state.pressed ? 1 : 0,
           s_dongle_blade_state.sequence,
           age_ms);

    s_dongle_blade_state.dirty = false;
}

static bool dongle_blade_pressed_fresh(int64_t now_us)
{
    return s_dongle_blade_state.valid &&
           s_dongle_blade_state.pressed &&
           dongle_blade_state_fresh(now_us);
}

#if DONGLE_ENABLE_USB_MOUSE
static float dongle_blade_gyro_axis_dps(const m2p_espnow_tracker_packet_t *packet,
                                         uint8_t axis)
{
    switch (axis) {
    case 0:
        return packet->gyro_dps[0];
    case 1:
        return packet->gyro_dps[1];
    case 2:
        return packet->gyro_dps[2];
    default:
        return 0.0f;
    }
}

static float dongle_blade_apply_gyro_deadzone(float gyro_dps, float deadzone_dps)
{
    return (gyro_dps > -deadzone_dps && gyro_dps < deadzone_dps) ? 0.0f : gyro_dps;
}

static int16_t dongle_blade_integrate_mouse_delta(float gyro_dps,
                                                   float dt_s,
                                                   float sensitivity,
                                                   int16_t max_step_delta,
                                                   float *remainder)
{
    const float delta_f = (gyro_dps * dt_s * sensitivity) + *remainder;
    if (delta_f >= ((float)max_step_delta + 0.5f)) {
        *remainder = 0.0f;
        return max_step_delta;
    }
    if (delta_f <= (-(float)max_step_delta - 0.5f)) {
        *remainder = 0.0f;
        return -max_step_delta;
    }

    const int16_t delta = (int16_t)((delta_f >= 0.0f) ? (delta_f + 0.5f) : (delta_f - 0.5f));
    *remainder = delta_f - (float)delta;
    return delta;
}

static bool dongle_get_fresh_chest_packet(int64_t now_us,
                                          const m2p_espnow_tracker_packet_t **out_packet,
                                          double *out_age_ms)
{
    dongle_latest_node_t *node = dongle_latest_node_for_id(DONGLE_BLADE_TURN_CHEST_NODE_ID);
    if (node == NULL || !node->valid || node->last_rx_us <= 0) {
        return false;
    }

    const int64_t age_us = now_us - node->last_rx_us;
    if (age_us < 0 ||
        age_us > ((int64_t)DONGLE_BLADE_TURN_CHEST_MAX_AGE_MS * 1000LL)) {
        return false;
    }

    if (out_packet != NULL) {
        *out_packet = &node->packet;
    }
    if (out_age_ms != NULL) {
        *out_age_ms = (double)age_us / 1000.0;
    }
    return true;
}
#endif

static void dongle_handle_blade_turn(int64_t now_us)
{
#if !DONGLE_ENABLE_USB_MOUSE
    (void)now_us;
    return;
#else
    if (!dongle_blade_pressed_fresh(now_us)) {
        s_dongle_last_blade_turn_us = 0;
        s_dongle_blade_turn_dx_remainder = 0.0f;
        s_dongle_blade_turn_dy_remainder = 0.0f;
        return;
    }

    if (s_dongle_last_blade_turn_us > 0) {
        const int64_t elapsed_us = now_us - s_dongle_last_blade_turn_us;
        if (elapsed_us >= 0 &&
            elapsed_us < ((int64_t)DONGLE_BLADE_TURN_PERIOD_MS * 1000LL)) {
            return;
        }
    }

    const m2p_espnow_tracker_packet_t *chest_packet = NULL;
    double chest_age_ms = -1.0;
    if (!dongle_get_fresh_chest_packet(now_us, &chest_packet, &chest_age_ms)) {
        s_dongle_last_blade_turn_us = 0;
        s_dongle_blade_turn_dx_remainder = 0.0f;
        s_dongle_blade_turn_dy_remainder = 0.0f;
        return;
    }

    if (s_dongle_last_blade_turn_us <= 0) {
        s_dongle_last_blade_turn_us = now_us;
        return;
    }

    const int64_t elapsed_us = now_us - s_dongle_last_blade_turn_us;
    if (elapsed_us <= 0) {
        return;
    }

    const float dt_s = (float)elapsed_us / 1000000.0f;
    const float turn_gyro_dps = dongle_blade_apply_gyro_deadzone(
        dongle_blade_gyro_axis_dps(chest_packet, DONGLE_BLADE_TURN_GYRO_AXIS) *
            DONGLE_BLADE_TURN_GYRO_SIGN,
        DONGLE_BLADE_TURN_DEADZONE_DPS);
    const float pitch_gyro_dps = dongle_blade_apply_gyro_deadzone(
        dongle_blade_gyro_axis_dps(chest_packet, DONGLE_BLADE_PITCH_GYRO_AXIS) *
            DONGLE_BLADE_PITCH_GYRO_SIGN,
        DONGLE_BLADE_PITCH_DEADZONE_DPS);

    const int16_t dx = dongle_blade_integrate_mouse_delta(
        turn_gyro_dps,
        dt_s,
        DONGLE_BLADE_TURN_SENSITIVITY,
        DONGLE_BLADE_TURN_MAX_STEP_DELTA,
        &s_dongle_blade_turn_dx_remainder);
    const int16_t dy = dongle_blade_integrate_mouse_delta(
        pitch_gyro_dps,
        dt_s,
        DONGLE_BLADE_PITCH_SENSITIVITY,
        DONGLE_BLADE_PITCH_MAX_STEP_DELTA,
        &s_dongle_blade_turn_dy_remainder);

    if (dx == 0 && dy == 0) {
        s_dongle_last_blade_turn_us = now_us;
        return;
    }

    esp_err_t err = ESP_OK;
    if (!usb_mouse_is_ready()) {
        return;
    }
    err = usb_mouse_move((int8_t)dx, (int8_t)dy, 0, 0);
    if (err == ESP_OK) {
        s_dongle_last_blade_turn_us = now_us;
    }

#if DONGLE_ENABLE_HID_EVENT_LOG
    printf("# blade-hid: pressed=1 chest_node=%u turn_axis=%d turn_dps=%.1f"
           " pitch_axis=%d pitch_dps=%.1f chest_age_ms=%.1f"
           " dx=%d dy=%d result=%s\n",
           (unsigned)DONGLE_BLADE_TURN_CHEST_NODE_ID,
           DONGLE_BLADE_TURN_GYRO_AXIS,
           (double)turn_gyro_dps,
           DONGLE_BLADE_PITCH_GYRO_AXIS,
           (double)pitch_gyro_dps,
           chest_age_ms,
           dx,
           dy,
           esp_err_to_name(err));
#endif
#endif
}

#if DONGLE_ENABLE_RAW_CSV_OUTPUT
static void dongle_print_latest_node(dongle_latest_node_t *node, int64_t now_us)
{
    const m2p_espnow_tracker_packet_t *packet = &node->packet;
    double age_ms = 0.0;
    if (node->last_rx_us > 0 && now_us >= node->last_rx_us) {
        age_ms = (double)(now_us - node->last_rx_us) / 1000.0;
    }

    /* Plain CSV for PC-side collection tools:
     * timestamp_ms,node_id,ax,ay,az,gx,gy,gz[,age_ms]
     */
#if DONGLE_ENABLE_SERIAL_AGE_COLUMN
    printf("%" PRIu32 ",%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.1f\n",
           packet->timestamp_us / 1000U,
           packet->node_id,
           (double)packet->accel_g[0],
           (double)packet->accel_g[1],
           (double)packet->accel_g[2],
           (double)packet->gyro_dps[0],
           (double)packet->gyro_dps[1],
           (double)packet->gyro_dps[2],
           age_ms);
#else
    printf("%" PRIu32 ",%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
           packet->timestamp_us / 1000U,
           packet->node_id,
           (double)packet->accel_g[0],
           (double)packet->accel_g[1],
           (double)packet->accel_g[2],
           (double)packet->gyro_dps[0],
           (double)packet->gyro_dps[1],
           (double)packet->gyro_dps[2]);
#endif

    node->dirty = false;
}
#endif

static void dongle_print_latest_states(void)
{
    const int64_t now_us = esp_timer_get_time();

#if DONGLE_ENABLE_RAW_CSV_OUTPUT
    for (size_t i = 0; i < DONGLE_MAX_TRACKER_NODES; i++) {
        dongle_latest_node_t *node = &s_dongle_latest_nodes[i];
        if (node->valid && node->dirty) {
            dongle_print_latest_node(node, now_us);
        }
    }
#else
    dongle_print_blade_state_if_changed(now_us);
#endif

    dongle_handle_blade_turn(now_us);
}

#if DONGLE_ENABLE_RF_INFERENCE

#if DONGLE_ENABLE_HID_ACTION_LOG
#define DONGLE_KEY_TAP_HOLD_MS        80
#define DONGLE_MOUSE_RIGHT_HOLD_MS    500
#define DONGLE_SLASH_CLICK_INTERVAL_MS 500
#define DONGLE_VIEW_ACTION_INTERVAL_MS 40
#define DONGLE_HAND_MOUSE_MOVE_DELTA  64
#define DONGLE_TURN_MOUSE_MOVE_DELTA  30
#define DONGLE_MOUSE_MOVE_STEP_DELTA  127
#define DONGLE_MOUSE_MOVE_STEP_DELAY_MS 0
#define DONGLE_OPPOSITE_TURN_BLOCK_MS 800
#define DONGLE_STATE_RESUME_GRACE_MS  200
#define DONGLE_STATE_EVENT_BRIDGE_MS  320
#define DONGLE_CONFIRM_FRAMES         3
#define DONGLE_MOVEMENT_CONFIRM_FRAMES 4
#define DONGLE_MOVEMENT_ENTRY_CONFIDENCE 0.52f
#define DONGLE_JUMP_MOVE_HOLD_GRACE_MS 200
#define DONGLE_INFER_RATE_HZ          25

#define DONGLE_ARRAY_SIZE(a)          (sizeof(a) / sizeof((a)[0]))

#define HID_KEY_A       0x04
#define HID_KEY_B       0x05
#define HID_KEY_C       0x06
#define HID_KEY_D       0x07
#define HID_KEY_E       0x08
#define HID_KEY_F       0x09
#define HID_KEY_G       0x0A
#define HID_KEY_H       0x0B
#define HID_KEY_I       0x0C
#define HID_KEY_J       0x0D
#define HID_KEY_K       0x0E
#define HID_KEY_L       0x0F
#define HID_KEY_M       0x10
#define HID_KEY_N       0x11
#define HID_KEY_O       0x12
#define HID_KEY_P       0x13
#define HID_KEY_Q       0x14
#define HID_KEY_R       0x15
#define HID_KEY_S       0x16
#define HID_KEY_T       0x17
#define HID_KEY_U       0x18
#define HID_KEY_V       0x19
#define HID_KEY_W       0x1A
#define HID_KEY_X       0x1B
#define HID_KEY_Y       0x1C
#define HID_KEY_Z       0x1D
#define HID_KEY_1       0x1E
#define HID_KEY_2       0x1F
#define HID_KEY_3       0x20
#define HID_KEY_4       0x21
#define HID_KEY_5       0x22
#define HID_KEY_6       0x23
#define HID_KEY_7       0x24
#define HID_KEY_8       0x25
#define HID_KEY_9       0x26
#define HID_KEY_0       0x27
#define HID_KEY_ENTER   0x28
#define HID_KEY_ESCAPE  0x29
#define HID_KEY_SPACE   0x2C

typedef enum {
    ACTION_TYPE_NONE,
    ACTION_TYPE_KEY_TAP,
    ACTION_TYPE_KEY_HOLD,
    ACTION_TYPE_CHARACTER_CYCLE,
    ACTION_TYPE_MOUSE_CLICK,
    ACTION_TYPE_MOUSE_RIGHT_TIMED_HOLD,
    ACTION_TYPE_MOUSE_HOLD,
    ACTION_TYPE_MOUSE_MOVE_LEFT,
    ACTION_TYPE_MOUSE_MOVE_RIGHT,
    ACTION_TYPE_MOUSE_TURN_LEFT,
    ACTION_TYPE_MOUSE_TURN_RIGHT,
    ACTION_TYPE_COUNT,
} dongle_action_type_t;

typedef enum {
    TRIGGER_COOLDOWN,
    TRIGGER_EDGE,
    TRIGGER_SUSTAIN,
    TRIGGER_REPEAT,
    TRIGGER_COUNT,
} dongle_trigger_mode_t;

typedef struct {
    dongle_action_type_t type;
    uint8_t modifier;
    uint8_t keycode;
    dongle_trigger_mode_t trigger;
    uint16_t cooldown_ms;
    uint16_t sustain_frames;
} dongle_key_action_t;

#define DONGLE_NUM_CLASSES RF_MODEL_CLASS_COUNT
#define DONGLE_IDLE_CLASS 3
#define DONGLE_JUMP_CLASS 4
#define DONGLE_LEFT_HAND_RAISE_CLASS 6
#define DONGLE_MOVE_NOISE_CLASS 7
#define DONGLE_RIGHT_HAND_RAISE_CLASS 8
#define DONGLE_RIGHT_HAND_SLASH_CLASS 9
#define DONGLE_RUN_CLASS 10
#define DONGLE_TURN_LEFT_CLASS 11
#define DONGLE_TURN_RIGHT_CLASS 12
#define DONGLE_WALK_CLASS 14

#define DONGLE_ACTION_CONFIG_MAGIC       0x4D325043U /* M2PC */
#define DONGLE_ACTION_CONFIG_VERSION     1
#define DONGLE_ACTION_CONFIG_NVS_NS      "m2p_dongle"
#define DONGLE_ACTION_CONFIG_NVS_KEY     "actions"
#define DONGLE_ACTION_CONFIG_POST_MAX    8192
#define DONGLE_RF_DEFAULT_CONFIDENCE_PERCENT ((uint8_t)(DONGLE_RF_MIN_CONFIDENCE * 100.0f + 0.5f))
#define DONGLE_RF_MIN_CONFIDENCE_PERCENT 30
#define DONGLE_RF_MAX_CONFIDENCE_PERCENT 95
#define DONGLE_MAX_COOLDOWN_MS           5000
#define DONGLE_MAX_SUSTAIN_FRAMES        25

typedef struct {
    uint8_t value;
    const char *label;
} dongle_value_label_t;

typedef struct {
    uint8_t type;
    uint8_t modifier;
    uint8_t keycode;
    uint8_t trigger;
    uint16_t cooldown_ms;
    uint16_t sustain_frames;
    uint8_t confidence_percent;
    uint8_t reserved;
} dongle_action_config_entry_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t entry_count;
    dongle_action_config_entry_t entries[DONGLE_NUM_CLASSES];
} dongle_action_config_storage_t;

static const dongle_value_label_t s_action_type_options[] = {
    { ACTION_TYPE_NONE, "禁用" },
    { ACTION_TYPE_KEY_TAP, "按一下键盘" },
    { ACTION_TYPE_KEY_HOLD, "按住键盘" },
    { ACTION_TYPE_CHARACTER_CYCLE, "角色键 1-4" },
    { ACTION_TYPE_MOUSE_CLICK, "鼠标左键点击" },
    { ACTION_TYPE_MOUSE_RIGHT_TIMED_HOLD, "鼠标右键定时按住" },
    { ACTION_TYPE_MOUSE_HOLD, "鼠标左键按住" },
    { ACTION_TYPE_MOUSE_MOVE_LEFT, "鼠标左移" },
    { ACTION_TYPE_MOUSE_MOVE_RIGHT, "鼠标右移" },
    { ACTION_TYPE_MOUSE_TURN_LEFT, "视角左转" },
    { ACTION_TYPE_MOUSE_TURN_RIGHT, "视角右转" },
};

static const dongle_value_label_t s_trigger_options[] = {
    { TRIGGER_COOLDOWN, "冷却" },
    { TRIGGER_EDGE, "刚出现" },
    { TRIGGER_SUSTAIN, "持续帧" },
    { TRIGGER_REPEAT, "重复" },
};

static const dongle_value_label_t s_modifier_options[] = {
    { 0, "无" },
    { USB_KEYBOARD_MOD_LEFT_SHIFT, "Shift" },
    { USB_KEYBOARD_MOD_LEFT_CTRL, "Ctrl" },
    { USB_KEYBOARD_MOD_LEFT_ALT, "Alt" },
    { USB_KEYBOARD_MOD_LEFT_SHIFT | USB_KEYBOARD_MOD_LEFT_CTRL, "Shift+Ctrl" },
};

static const dongle_value_label_t s_key_options[] = {
    { 0, "无" },
    { HID_KEY_A, "A" }, { HID_KEY_B, "B" }, { HID_KEY_C, "C" }, { HID_KEY_D, "D" },
    { HID_KEY_E, "E" }, { HID_KEY_F, "F" }, { HID_KEY_G, "G" }, { HID_KEY_H, "H" },
    { HID_KEY_I, "I" }, { HID_KEY_J, "J" }, { HID_KEY_K, "K" }, { HID_KEY_L, "L" },
    { HID_KEY_M, "M" }, { HID_KEY_N, "N" }, { HID_KEY_O, "O" }, { HID_KEY_P, "P" },
    { HID_KEY_Q, "Q" }, { HID_KEY_R, "R" }, { HID_KEY_S, "S" }, { HID_KEY_T, "T" },
    { HID_KEY_U, "U" }, { HID_KEY_V, "V" }, { HID_KEY_W, "W" }, { HID_KEY_X, "X" },
    { HID_KEY_Y, "Y" }, { HID_KEY_Z, "Z" },
    { HID_KEY_1, "1" }, { HID_KEY_2, "2" }, { HID_KEY_3, "3" }, { HID_KEY_4, "4" },
    { HID_KEY_5, "5" }, { HID_KEY_6, "6" }, { HID_KEY_7, "7" }, { HID_KEY_8, "8" },
    { HID_KEY_9, "9" }, { HID_KEY_0, "0" },
    { HID_KEY_SPACE, "空格" }, { HID_KEY_ENTER, "回车" }, { HID_KEY_ESCAPE, "Esc" },
};

static const char *s_dongle_action_display_names[DONGLE_NUM_CLASSES] = {
    [0] = "双手交叉额头",
    [1] = "下压",
    [2] = "射击",
    [3] = "空闲",
    [4] = "跳跃",
    [5] = "踢",
    [6] = "左手抬起",
    [7] = "动作噪声",
    [8] = "右手抬起",
    [9] = "右手挥砍",
    [10] = "奔跑",
    [11] = "左转",
    [12] = "右转",
    [13] = "奥特曼光线",
    [14] = "行走",
};

/* RF class order from sklearn string labels:
   hands_cross_forehead(0), hands_press_down(1), hands_shoot(2), idle(3),
   jump(4), kick(5), left_hand_raise(6), move_noise(7),
   right_hand_raise(8), right_hand_slash(9), run(10), turn_left(11),
   turn_right(12), ultraman_beam(13), walk(14) */
static const dongle_key_action_t s_default_class_key_actions[DONGLE_NUM_CLASSES] = {
    [0] = { ACTION_TYPE_CHARACTER_CYCLE, 0, 0, TRIGGER_EDGE, 800, 0 },       /* hands_cross_forehead -> 1/2/3/4 */
    [1] = { ACTION_TYPE_KEY_TAP, 0, HID_KEY_F, TRIGGER_COOLDOWN, 600, 0 },   /* hands_press_down -> F */
    [2] = { ACTION_TYPE_MOUSE_RIGHT_TIMED_HOLD, 0, 0, TRIGGER_EDGE, 500, 0 }, /* hands_shoot -> hold right mouse 500ms */
    [3] = { ACTION_TYPE_NONE, 0, 0, TRIGGER_COOLDOWN, 0, 0 },                /* idle */
    [4] = { ACTION_TYPE_KEY_TAP, 0, HID_KEY_SPACE, TRIGGER_EDGE, 1000, 0 },  /* jump -> SPACE */
    [5] = { ACTION_TYPE_KEY_TAP, 0, HID_KEY_E, TRIGGER_COOLDOWN, 1000, 0 },  /* kick -> E */
    [6] = { ACTION_TYPE_KEY_TAP, 0, HID_KEY_M, TRIGGER_EDGE, 800, 0 },        /* left_hand_raise -> M */
    [7] = { ACTION_TYPE_NONE, 0, 0, TRIGGER_COOLDOWN, 0, 0 },                /* move_noise */
    [8] = { ACTION_TYPE_KEY_TAP, 0, HID_KEY_X, TRIGGER_EDGE, 800, 0 },        /* right_hand_raise -> X */
    [9] = { ACTION_TYPE_MOUSE_CLICK, 0, 0, TRIGGER_REPEAT, DONGLE_SLASH_CLICK_INTERVAL_MS, 1 }, /* right_hand_slash -> left click every 500ms */
    [10] = { ACTION_TYPE_KEY_HOLD, USB_KEYBOARD_MOD_LEFT_SHIFT, HID_KEY_W, TRIGGER_COOLDOWN, 0, 0 }, /* run -> Shift+W */
    [11] = { ACTION_TYPE_MOUSE_TURN_LEFT, 0, 0, TRIGGER_REPEAT, DONGLE_VIEW_ACTION_INTERVAL_MS, 1 }, /* turn_left -> mouse left */
    [12] = { ACTION_TYPE_MOUSE_TURN_RIGHT, 0, 0, TRIGGER_REPEAT, DONGLE_VIEW_ACTION_INTERVAL_MS, 1 }, /* turn_right -> mouse right */
    [13] = { ACTION_TYPE_KEY_TAP, 0, HID_KEY_Q, TRIGGER_COOLDOWN, 1000, 0 }, /* ultraman_beam -> Q */
    [14] = { ACTION_TYPE_KEY_HOLD, 0, HID_KEY_W, TRIGGER_COOLDOWN, 0, 0 },   /* walk -> W */
};

static dongle_key_action_t s_dongle_key_actions[DONGLE_NUM_CLASSES];
static uint8_t s_dongle_action_confidence_percent[DONGLE_NUM_CLASSES];
static bool s_dongle_action_config_ready = false;

static const char *dongle_find_option_label(const dongle_value_label_t *options,
                                            size_t option_count,
                                            uint8_t value,
                                            const char *fallback)
{
    for (size_t i = 0; i < option_count; i++) {
        if (options[i].value == value) {
            return options[i].label;
        }
    }

    return fallback;
}

static const char *dongle_action_type_name(uint8_t type)
{
    return dongle_find_option_label(s_action_type_options,
                                    DONGLE_ARRAY_SIZE(s_action_type_options),
                                    type,
                                    "未知");
}

static const char *dongle_trigger_name(uint8_t trigger)
{
    return dongle_find_option_label(s_trigger_options,
                                    DONGLE_ARRAY_SIZE(s_trigger_options),
                                    trigger,
                                    "未知");
}

static const char *dongle_key_name(uint8_t keycode)
{
    return dongle_find_option_label(s_key_options,
                                    DONGLE_ARRAY_SIZE(s_key_options),
                                    keycode,
                                    "未知");
}

static bool dongle_option_value_exists(const dongle_value_label_t *options,
                                       size_t option_count,
                                       uint8_t value)
{
    for (size_t i = 0; i < option_count; i++) {
        if (options[i].value == value) {
            return true;
        }
    }

    return false;
}

static uint8_t dongle_clamp_u8(uint32_t value, uint8_t min_value, uint8_t max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }

    return (uint8_t)value;
}

static uint16_t dongle_clamp_u16(uint32_t value, uint16_t min_value, uint16_t max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }

    return (uint16_t)value;
}

static void dongle_action_config_set_defaults(void)
{
    memcpy(s_dongle_key_actions, s_default_class_key_actions, sizeof(s_dongle_key_actions));
    for (uint8_t i = 0; i < DONGLE_NUM_CLASSES; i++) {
        s_dongle_action_confidence_percent[i] = DONGLE_RF_DEFAULT_CONFIDENCE_PERCENT;
    }
    s_dongle_action_config_ready = true;
}

static void dongle_action_config_ensure_ready(void)
{
    if (!s_dongle_action_config_ready) {
        dongle_action_config_set_defaults();
    }
}

static bool dongle_action_config_entry_valid(const dongle_action_config_entry_t *entry)
{
    return entry->type < ACTION_TYPE_COUNT &&
           entry->trigger < TRIGGER_COUNT &&
           dongle_option_value_exists(s_key_options, DONGLE_ARRAY_SIZE(s_key_options), entry->keycode) &&
           dongle_option_value_exists(s_modifier_options, DONGLE_ARRAY_SIZE(s_modifier_options), entry->modifier) &&
           entry->confidence_percent >= DONGLE_RF_MIN_CONFIDENCE_PERCENT &&
           entry->confidence_percent <= DONGLE_RF_MAX_CONFIDENCE_PERCENT &&
           entry->cooldown_ms <= DONGLE_MAX_COOLDOWN_MS &&
           entry->sustain_frames <= DONGLE_MAX_SUSTAIN_FRAMES;
}

static void dongle_action_config_export(dongle_action_config_storage_t *storage)
{
    dongle_action_config_ensure_ready();
    memset(storage, 0, sizeof(*storage));
    storage->magic = DONGLE_ACTION_CONFIG_MAGIC;
    storage->version = DONGLE_ACTION_CONFIG_VERSION;
    storage->entry_count = DONGLE_NUM_CLASSES;

    for (uint8_t i = 0; i < DONGLE_NUM_CLASSES; i++) {
        const dongle_key_action_t *action = &s_dongle_key_actions[i];
        dongle_action_config_entry_t *entry = &storage->entries[i];
        entry->type = (uint8_t)action->type;
        entry->modifier = action->modifier;
        entry->keycode = action->keycode;
        entry->trigger = (uint8_t)action->trigger;
        entry->cooldown_ms = action->cooldown_ms;
        entry->sustain_frames = action->sustain_frames;
        entry->confidence_percent = s_dongle_action_confidence_percent[i];
    }
}

static void dongle_action_config_apply(const dongle_action_config_storage_t *storage)
{
    dongle_action_config_set_defaults();

    if (storage == NULL ||
        storage->magic != DONGLE_ACTION_CONFIG_MAGIC ||
        storage->version != DONGLE_ACTION_CONFIG_VERSION ||
        storage->entry_count != DONGLE_NUM_CLASSES) {
        return;
    }

    for (uint8_t i = 0; i < DONGLE_NUM_CLASSES; i++) {
        const dongle_action_config_entry_t *entry = &storage->entries[i];
        if (!dongle_action_config_entry_valid(entry)) {
            ESP_LOGW(TAG, "invalid action config entry %u, using default", i);
            continue;
        }

        s_dongle_key_actions[i] = (dongle_key_action_t) {
            .type = (dongle_action_type_t)entry->type,
            .modifier = entry->modifier,
            .keycode = entry->keycode,
            .trigger = (dongle_trigger_mode_t)entry->trigger,
            .cooldown_ms = entry->cooldown_ms,
            .sustain_frames = entry->sustain_frames,
        };
        s_dongle_action_confidence_percent[i] = entry->confidence_percent;
    }
}

static esp_err_t dongle_action_config_save_current(void)
{
    dongle_action_config_storage_t storage;
    dongle_action_config_export(&storage);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(DONGLE_ACTION_CONFIG_NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(handle, DONGLE_ACTION_CONFIG_NVS_KEY, &storage, sizeof(storage));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t dongle_action_config_load(void)
{
    dongle_action_config_set_defaults();

    nvs_handle_t handle;
    esp_err_t err = nvs_open(DONGLE_ACTION_CONFIG_NVS_NS, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "dongle action config not found, using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open dongle action config failed: %s", esp_err_to_name(err));
        return err;
    }

    dongle_action_config_storage_t storage = {0};
    size_t required_size = sizeof(storage);
    err = nvs_get_blob(handle, DONGLE_ACTION_CONFIG_NVS_KEY, &storage, &required_size);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "dongle action config blob not found, using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read dongle action config failed: %s", esp_err_to_name(err));
        return err;
    }
    if (required_size != sizeof(storage)) {
        ESP_LOGW(TAG, "dongle action config size mismatch, using defaults");
        return ESP_ERR_INVALID_SIZE;
    }

    dongle_action_config_apply(&storage);
    ESP_LOGI(TAG, "dongle action config loaded from NVS");
    return ESP_OK;
}

static esp_err_t dongle_action_config_reset_defaults(void)
{
    dongle_action_config_set_defaults();
    return dongle_action_config_save_current();
}

static float dongle_action_min_confidence(uint8_t class_idx)
{
    dongle_action_config_ensure_ready();
    if (class_idx >= DONGLE_NUM_CLASSES) {
        return DONGLE_RF_MIN_CONFIDENCE;
    }

    return (float)s_dongle_action_confidence_percent[class_idx] / 100.0f;
}

#if DONGLE_ENABLE_USB_KEYBOARD
#define DONGLE_HID_LOG_PREFIX "# hid"
#else
#define DONGLE_HID_LOG_PREFIX "# hid-dry-run"
#endif

static const char *dongle_action_class_name(uint8_t class_idx)
{
    return (class_idx < RF_MODEL_CLASS_COUNT) ? rf_model_class_names[class_idx] : "unknown";
}

static const char *dongle_action_class_display_name(uint8_t class_idx)
{
    if (class_idx < DONGLE_NUM_CLASSES && s_dongle_action_display_names[class_idx] != NULL) {
        return s_dongle_action_display_names[class_idx];
    }

    return "未知动作";
}

#if DONGLE_ENABLE_USB_TELEMETRY
typedef struct {
    uint8_t action_class;
    uint8_t confidence_percent;
    uint8_t intensity_percent;
    bool active;
    int64_t last_inference_us;
    uint32_t event_count;
    uint8_t event_action_class;
    const char *event_name;
} dongle_telemetry_state_t;

static dongle_telemetry_state_t s_dongle_telemetry_state = {
    .action_class = DONGLE_IDLE_CLASS,
    .event_action_class = DONGLE_IDLE_CLASS,
    .event_name = "none",
};
static uint32_t s_dongle_telemetry_packet_sequence;
static int64_t s_dongle_last_telemetry_send_us;
static float s_dongle_smoothed_motion_intensity;
static float dongle_effective_min_confidence(uint8_t class_idx);

static void dongle_telemetry_update_inference(uint8_t class_idx,
                                              float confidence,
                                              uint8_t intensity_percent,
                                              int64_t now_us)
{
    const bool confident = class_idx < DONGLE_NUM_CLASSES &&
                           confidence >= dongle_effective_min_confidence(class_idx);
    const uint8_t display_class = confident ? class_idx : DONGLE_IDLE_CLASS;

    s_dongle_telemetry_state.action_class = display_class;
    s_dongle_telemetry_state.confidence_percent =
        (uint8_t)(confidence <= 0.0f ? 0U :
                  confidence >= 1.0f ? 100U :
                  (uint8_t)(confidence * 100.0f + 0.5f));
    s_dongle_telemetry_state.intensity_percent = intensity_percent;
    s_dongle_telemetry_state.active = confident &&
                                      display_class != DONGLE_IDLE_CLASS &&
                                      display_class != DONGLE_MOVE_NOISE_CLASS;
    s_dongle_telemetry_state.last_inference_us = now_us;
}

static bool dongle_telemetry_event_is_countable(const char *event)
{
    return strcmp(event, "key_tap") == 0 ||
           strcmp(event, "key_tap_with_movement") == 0 ||
           strcmp(event, "character_cycle") == 0 ||
           strcmp(event, "mouse_click") == 0 ||
           strcmp(event, "mouse_hold") == 0;
}

static void dongle_telemetry_note_output(uint8_t class_idx, const char *event)
{
    if (event == NULL ||
        strcmp(event, "key_release") == 0 ||
        strcmp(event, "mouse_release") == 0) {
        return;
    }

    s_dongle_telemetry_state.event_action_class = class_idx;
    s_dongle_telemetry_state.event_name = event;
    if (dongle_telemetry_event_is_countable(event)) {
        s_dongle_telemetry_state.event_count++;
    }
}
#endif

#if DONGLE_ENABLE_ACTION_DEBUG_OUTPUT
static bool s_dongle_hid_trigger_since_last_print = false;
static uint32_t s_dongle_hid_trigger_count_since_last_print = 0;
static uint8_t s_dongle_last_hid_trigger_class = DONGLE_IDLE_CLASS;
static const char *s_dongle_last_hid_trigger_event = "none";
#endif
static bool s_dongle_hid_ready = false;
static const char *s_dongle_hid_status = "not_checked";

static void dongle_note_hid_trigger(uint8_t class_idx, const char *event)
{
#if DONGLE_ENABLE_ACTION_DEBUG_OUTPUT
    s_dongle_hid_trigger_since_last_print = true;
    s_dongle_hid_trigger_count_since_last_print++;
    s_dongle_last_hid_trigger_class = class_idx;
    s_dongle_last_hid_trigger_event = event;
    s_dongle_hid_status = "fired";
#else
    (void)class_idx;
    (void)event;
#endif
}

static void dongle_log_key_event(uint8_t class_idx,
                                 const char *event,
                                 uint8_t modifier,
                                 uint8_t keycode,
                                 esp_err_t err)
{
    dongle_note_hid_trigger(class_idx, event);
#if DONGLE_ENABLE_USB_TELEMETRY
    if (err == ESP_OK) {
        dongle_telemetry_note_output(class_idx, event);
    }
#endif
#if DONGLE_ENABLE_HID_EVENT_LOG
#if DONGLE_ENABLE_USB_KEYBOARD
    printf("%s: action=%s event=%s modifier=0x%02x key=%s result=%s\n",
           DONGLE_HID_LOG_PREFIX,
           dongle_action_class_name(class_idx),
           event,
           modifier,
           dongle_key_name(keycode),
           esp_err_to_name(err));
#else
    (void)err;
    printf("%s: action=%s event=%s modifier=0x%02x key=%s\n",
           DONGLE_HID_LOG_PREFIX,
           dongle_action_class_name(class_idx),
           event,
           modifier,
           dongle_key_name(keycode));
#endif
#else
    (void)modifier;
    (void)keycode;
    (void)err;
#endif
}

static void dongle_log_mouse_button_event(uint8_t class_idx,
                                          const char *event,
                                          uint8_t buttons,
                                          esp_err_t err)
{
    dongle_note_hid_trigger(class_idx, event);
#if DONGLE_ENABLE_USB_TELEMETRY
    if (err == ESP_OK) {
        dongle_telemetry_note_output(class_idx, event);
    }
#endif
#if DONGLE_ENABLE_HID_EVENT_LOG
#if DONGLE_ENABLE_USB_MOUSE
    printf("%s: action=%s event=%s buttons=0x%02x result=%s\n",
           DONGLE_HID_LOG_PREFIX,
           dongle_action_class_name(class_idx),
           event,
           buttons,
           esp_err_to_name(err));
#else
    (void)err;
    printf("%s: action=%s event=%s buttons=0x%02x\n",
           DONGLE_HID_LOG_PREFIX,
           dongle_action_class_name(class_idx),
           event,
           buttons);
#endif
#else
    (void)buttons;
    (void)err;
#endif
}

static void dongle_log_mouse_move_event(uint8_t class_idx, int16_t dx, esp_err_t err)
{
    dongle_note_hid_trigger(class_idx, "mouse_move");
#if DONGLE_ENABLE_USB_TELEMETRY
    if (err == ESP_OK) {
        dongle_telemetry_note_output(class_idx, "mouse_move");
    }
#endif
#if DONGLE_ENABLE_HID_EVENT_LOG
#if DONGLE_ENABLE_USB_MOUSE
    printf("%s: action=%s event=mouse_move dx=%d result=%s\n",
           DONGLE_HID_LOG_PREFIX,
           dongle_action_class_name(class_idx),
           dx,
           esp_err_to_name(err));
#else
    (void)err;
    printf("%s: action=%s event=mouse_move dx=%d\n",
           DONGLE_HID_LOG_PREFIX,
           dongle_action_class_name(class_idx),
           dx);
#endif
#else
    (void)dx;
    (void)err;
#endif
}

static esp_err_t dongle_mouse_move_x(int16_t dx)
{
    esp_err_t err = ESP_OK;
#if DONGLE_ENABLE_USB_MOUSE
    int16_t remaining = dx;
    while (remaining != 0) {
        int8_t step = 0;
        if (remaining > DONGLE_MOUSE_MOVE_STEP_DELTA) {
            step = DONGLE_MOUSE_MOVE_STEP_DELTA;
        } else if (remaining < -DONGLE_MOUSE_MOVE_STEP_DELTA) {
            step = -DONGLE_MOUSE_MOVE_STEP_DELTA;
        } else {
            step = (int8_t)remaining;
        }

        err = usb_mouse_move(step, 0, 0, 0);
        if (err != ESP_OK) {
            break;
        }
        remaining = (int16_t)(remaining - step);
        if (remaining != 0 && DONGLE_MOUSE_MOVE_STEP_DELAY_MS > 0) {
            vTaskDelay(pdMS_TO_TICKS(DONGLE_MOUSE_MOVE_STEP_DELAY_MS));
        }
    }
#else
    (void)dx;
#endif
    return err;
}

static int8_t s_dongle_keyboard_hold_class = -1;
static int8_t s_dongle_mouse_hold_class = -1;
static int8_t s_dongle_mouse_timed_hold_class = -1;
static int64_t s_dongle_mouse_timed_release_us = 0;
static int8_t s_dongle_confirmed_class = -1;
static int8_t s_dongle_pending_class = -1;
static uint8_t s_dongle_pending_count = 0;

static int64_t s_dongle_last_fire_us[DONGLE_NUM_CLASSES] = {0};
static int8_t s_dongle_last_turn_fire_class = -1;
static int64_t s_dongle_last_turn_fire_us = 0;
static int8_t s_dongle_last_state_class = DONGLE_IDLE_CLASS;
static int64_t s_dongle_last_state_us = 0;
static int8_t s_dongle_resume_state_class = -1;
static int64_t s_dongle_resume_state_until_us = 0;
static int64_t s_dongle_last_blade_pressed_us = 0;
static int64_t s_dongle_last_jump_move_fire_us = 0;
static bool s_dongle_edge_armed[DONGLE_NUM_CLASSES] = {0};
static bool s_dongle_edge_armed_ready = false;
static uint16_t s_dongle_sustain_count[DONGLE_NUM_CLASSES] = {0};
static uint8_t s_dongle_character_slot = 0;

static void dongle_init_edge_armed(void)
{
    if (s_dongle_edge_armed_ready) {
        return;
    }

    for (uint8_t i = 0; i < DONGLE_NUM_CLASSES; i++) {
        s_dongle_edge_armed[i] = true;
    }
    s_dongle_edge_armed_ready = true;
}

static void dongle_release_keyboard_hold(void)
{
    if (s_dongle_keyboard_hold_class >= 0) {
        const uint8_t class_idx = (uint8_t)s_dongle_keyboard_hold_class;
        esp_err_t err = ESP_OK;
#if DONGLE_ENABLE_USB_KEYBOARD
        err = usb_keyboard_release();
#endif
        dongle_log_key_event(class_idx, "key_release", 0, 0, err);
        s_dongle_keyboard_hold_class = -1;
    }
}

static void dongle_release_mouse_hold(void)
{
    if (s_dongle_mouse_hold_class >= 0) {
        const uint8_t class_idx = (uint8_t)s_dongle_mouse_hold_class;
        esp_err_t err = ESP_OK;
#if DONGLE_ENABLE_USB_MOUSE
        err = usb_mouse_release_buttons();
#endif
        dongle_log_mouse_button_event(class_idx, "mouse_release", 0, err);
        s_dongle_mouse_hold_class = -1;
    }
}

static void dongle_release_timed_mouse_hold(bool force, int64_t now_us)
{
    if (s_dongle_mouse_timed_hold_class < 0) {
        return;
    }

    if (!force && now_us < s_dongle_mouse_timed_release_us) {
        return;
    }

    const uint8_t class_idx = (uint8_t)s_dongle_mouse_timed_hold_class;
    esp_err_t err = ESP_OK;
#if DONGLE_ENABLE_USB_MOUSE
    err = usb_mouse_release_buttons();
#endif
    dongle_log_mouse_button_event(class_idx, "mouse_release", 0, err);
    s_dongle_mouse_timed_hold_class = -1;
    s_dongle_mouse_timed_release_us = 0;
}

static void dongle_release_hold_actions(void)
{
    dongle_release_keyboard_hold();
    dongle_release_mouse_hold();
    dongle_release_timed_mouse_hold(true, 0);
}

static bool dongle_is_turn_class(uint8_t class_idx)
{
    return class_idx == DONGLE_TURN_LEFT_CLASS || class_idx == DONGLE_TURN_RIGHT_CLASS;
}

static bool dongle_is_view_move_class(uint8_t class_idx)
{
    return class_idx == DONGLE_TURN_LEFT_CLASS ||
           class_idx == DONGLE_TURN_RIGHT_CLASS;
}

static bool dongle_is_state_class(uint8_t class_idx)
{
    return class_idx == DONGLE_IDLE_CLASS ||
           class_idx == DONGLE_MOVE_NOISE_CLASS ||
           class_idx == DONGLE_RIGHT_HAND_SLASH_CLASS ||
           class_idx == DONGLE_RUN_CLASS ||
           class_idx == DONGLE_WALK_CLASS;
}

static bool dongle_is_active_state_class(uint8_t class_idx)
{
    return class_idx == DONGLE_RIGHT_HAND_SLASH_CLASS ||
           class_idx == DONGLE_RUN_CLASS ||
           class_idx == DONGLE_WALK_CLASS;
}

static bool dongle_is_movement_hold_class(uint8_t class_idx)
{
    return class_idx == DONGLE_RUN_CLASS || class_idx == DONGLE_WALK_CLASS;
}

static float dongle_effective_min_confidence(uint8_t class_idx)
{
    const float configured_confidence = dongle_action_min_confidence(class_idx);
    if (dongle_is_movement_hold_class(class_idx) &&
        configured_confidence > DONGLE_MOVEMENT_ENTRY_CONFIDENCE) {
        return DONGLE_MOVEMENT_ENTRY_CONFIDENCE;
    }

    return configured_confidence;
}

static bool dongle_is_event_class(uint8_t class_idx)
{
    return class_idx < DONGLE_NUM_CLASSES && !dongle_is_state_class(class_idx);
}

static bool dongle_class_from_state_label(const char *label, uint8_t *out_class)
{
    if (label == NULL || out_class == NULL) {
        return false;
    }
    if (strcmp(label, "idle") == 0) {
        *out_class = DONGLE_IDLE_CLASS;
        return true;
    }
    if (strcmp(label, "move_noise") == 0) {
        *out_class = DONGLE_MOVE_NOISE_CLASS;
        return true;
    }
    if (strcmp(label, "right_hand_slash") == 0) {
        *out_class = DONGLE_RIGHT_HAND_SLASH_CLASS;
        return true;
    }
    if (strcmp(label, "run") == 0) {
        *out_class = DONGLE_RUN_CLASS;
        return true;
    }
    if (strcmp(label, "walk") == 0) {
        *out_class = DONGLE_WALK_CLASS;
        return true;
    }
    return false;
}

static bool dongle_is_mouse_view_action(const dongle_key_action_t *action)
{
    return action->type == ACTION_TYPE_MOUSE_MOVE_LEFT ||
           action->type == ACTION_TYPE_MOUSE_MOVE_RIGHT ||
           action->type == ACTION_TYPE_MOUSE_TURN_LEFT ||
           action->type == ACTION_TYPE_MOUSE_TURN_RIGHT;
}

static bool dongle_is_opposite_turn(uint8_t class_idx, int8_t previous_class)
{
    return (class_idx == DONGLE_TURN_LEFT_CLASS && previous_class == DONGLE_TURN_RIGHT_CLASS) ||
           (class_idx == DONGLE_TURN_RIGHT_CLASS && previous_class == DONGLE_TURN_LEFT_CLASS);
}

static bool dongle_should_block_opposite_turn(uint8_t class_idx, int64_t now_us)
{
    if (!dongle_is_turn_class(class_idx) ||
        !dongle_is_opposite_turn(class_idx, s_dongle_last_turn_fire_class)) {
        return false;
    }

    const int64_t elapsed_us = now_us - s_dongle_last_turn_fire_us;
    return elapsed_us >= 0 &&
           elapsed_us < ((int64_t)DONGLE_OPPOSITE_TURN_BLOCK_MS * 1000LL);
}

static void dongle_remember_state_class(uint8_t class_idx, int64_t now_us)
{
    if (!dongle_is_state_class(class_idx)) {
        return;
    }

    s_dongle_last_state_class = (int8_t)class_idx;
    s_dongle_last_state_us = now_us;
    s_dongle_resume_state_class = -1;
    s_dongle_resume_state_until_us = 0;
}

static void dongle_note_event_after_state(uint8_t class_idx, int64_t now_us)
{
    if (!dongle_is_event_class(class_idx)) {
        return;
    }

    if (s_dongle_resume_state_class < 0) {
        const int64_t elapsed_us = now_us - s_dongle_last_state_us;
        if (s_dongle_last_state_class >= 0 &&
            elapsed_us >= 0 &&
            elapsed_us <= ((int64_t)DONGLE_STATE_EVENT_BRIDGE_MS * 1000LL)) {
            s_dongle_resume_state_class = s_dongle_last_state_class;
        }
    }

    if (s_dongle_resume_state_class >= 0) {
        s_dongle_resume_state_until_us =
            now_us + ((int64_t)DONGLE_STATE_RESUME_GRACE_MS * 1000LL);
    }
}

static bool dongle_try_resume_state_after_event(uint8_t *class_idx, int64_t now_us)
{
    if (s_dongle_resume_state_class < 0) {
        return false;
    }

    if (now_us > s_dongle_resume_state_until_us) {
        s_dongle_resume_state_class = -1;
        s_dongle_resume_state_until_us = 0;
        return false;
    }

    const uint8_t resume_class = (uint8_t)s_dongle_resume_state_class;
    if (!dongle_is_state_class(resume_class) || dongle_is_active_state_class(*class_idx)) {
        return false;
    }

    *class_idx = resume_class;
    s_dongle_hid_status = "state_resume_grace";
    return true;
}

static bool dongle_try_keep_movement_after_blade(uint8_t *class_idx, int64_t now_us)
{
    if (class_idx == NULL || s_dongle_keyboard_hold_class < 0) {
        return false;
    }

    const uint8_t hold_class = (uint8_t)s_dongle_keyboard_hold_class;
    if (!dongle_is_movement_hold_class(hold_class)) {
        return false;
    }

    if (*class_idx != DONGLE_IDLE_CLASS && *class_idx != DONGLE_MOVE_NOISE_CLASS) {
        return false;
    }

    const int64_t elapsed_us = now_us - s_dongle_last_blade_pressed_us;
    if (elapsed_us < 0 ||
        elapsed_us > ((int64_t)DONGLE_BLADE_MOVE_RELEASE_GRACE_MS * 1000LL)) {
        return false;
    }

    *class_idx = hold_class;
    s_dongle_hid_status = "blade_move_hold_grace";
    return true;
}

static uint8_t dongle_smooth_class(uint8_t raw_class)
{
    if ((int8_t)raw_class == s_dongle_pending_class) {
        if (s_dongle_pending_count < 255) {
            s_dongle_pending_count++;
        }
    } else {
        s_dongle_pending_class = (int8_t)raw_class;
        s_dongle_pending_count = 1;
    }

    const uint8_t required_frames =
        dongle_is_movement_hold_class(raw_class) &&
        s_dongle_confirmed_class != (int8_t)raw_class ?
        DONGLE_MOVEMENT_CONFIRM_FRAMES :
        DONGLE_CONFIRM_FRAMES;
    if (s_dongle_pending_count >= required_frames) {
        s_dongle_confirmed_class = s_dongle_pending_class;
    }

    return (s_dongle_confirmed_class >= 0) ? (uint8_t)s_dongle_confirmed_class : DONGLE_IDLE_CLASS;
}

static bool dongle_action_sustain_ready(uint8_t class_idx, const dongle_key_action_t *action)
{
    return action->sustain_frames == 0 ||
           s_dongle_sustain_count[class_idx] >= action->sustain_frames;
}

static bool dongle_action_sustain_fire_frame(uint8_t class_idx, const dongle_key_action_t *action)
{
    const uint16_t required_frames = (action->sustain_frames == 0) ?
                                     1 :
                                     action->sustain_frames;
    return s_dongle_sustain_count[class_idx] == required_frames;
}

static int8_t dongle_jump_movement_class(int64_t now_us)
{
    if (s_dongle_keyboard_hold_class >= 0 &&
        dongle_is_movement_hold_class((uint8_t)s_dongle_keyboard_hold_class)) {
        return s_dongle_keyboard_hold_class;
    }

    if (s_dongle_resume_state_class >= 0 &&
        now_us <= s_dongle_resume_state_until_us &&
        dongle_is_movement_hold_class((uint8_t)s_dongle_resume_state_class)) {
        return s_dongle_resume_state_class;
    }

    return -1;
}

static esp_err_t dongle_tap_key_with_movement(uint8_t movement_class,
                                               const dongle_key_action_t *tap_action)
{
    const dongle_key_action_t *movement_action = &s_dongle_key_actions[movement_class];
    const uint8_t modifier = movement_action->modifier | tap_action->modifier;
    const uint8_t keycodes[6] = {
        movement_action->keycode,
        tap_action->keycode,
        0, 0, 0, 0,
    };

    esp_err_t err = usb_keyboard_press_keys(modifier, keycodes);
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(DONGLE_KEY_TAP_HOLD_MS));

    const uint8_t movement_keys[6] = { movement_action->keycode, 0, 0, 0, 0, 0 };
    err = usb_keyboard_press_keys(movement_action->modifier, movement_keys);
    if (err == ESP_OK) {
        s_dongle_keyboard_hold_class = (int8_t)movement_class;
    } else {
        /* Force the normal HOLD path to retry the movement report next frame. */
        s_dongle_keyboard_hold_class = -1;
    }
    return err;
}

static void dongle_fire_action(uint8_t class_idx,
                               const dongle_key_action_t *action,
                               int64_t now_us,
                               int8_t jump_movement_class)
{
    if (action->type == ACTION_TYPE_KEY_TAP) {
        esp_err_t err = ESP_OK;
#if DONGLE_ENABLE_USB_KEYBOARD
        if (class_idx == DONGLE_JUMP_CLASS && jump_movement_class >= 0) {
            err = dongle_tap_key_with_movement((uint8_t)jump_movement_class, action);
            if (err == ESP_OK) {
                s_dongle_last_jump_move_fire_us = now_us;
            }
        } else {
            err = usb_keyboard_tap_key(action->modifier, action->keycode, DONGLE_KEY_TAP_HOLD_MS);
        }
#endif
        const uint8_t logged_modifier =
            (class_idx == DONGLE_JUMP_CLASS && jump_movement_class >= 0) ?
            (uint8_t)(s_dongle_key_actions[jump_movement_class].modifier | action->modifier) :
            action->modifier;
        dongle_log_key_event(class_idx,
                             jump_movement_class >= 0 ? "key_tap_with_movement" : "key_tap",
                             logged_modifier,
                             action->keycode,
                             err);
    } else if (action->type == ACTION_TYPE_CHARACTER_CYCLE) {
        static const uint8_t character_keys[] = {HID_KEY_1, HID_KEY_2, HID_KEY_3, HID_KEY_4};
        const uint8_t keycode = character_keys[s_dongle_character_slot];
        esp_err_t err = ESP_OK;
#if DONGLE_ENABLE_USB_KEYBOARD
        err = usb_keyboard_tap_key(0, keycode, DONGLE_KEY_TAP_HOLD_MS);
#endif
        dongle_log_key_event(class_idx, "character_cycle", 0, keycode, err);
        if (err == ESP_OK) {
            s_dongle_character_slot = (uint8_t)((s_dongle_character_slot + 1U) %
                                                DONGLE_ARRAY_SIZE(character_keys));
        }
    } else if (action->type == ACTION_TYPE_MOUSE_CLICK) {
        esp_err_t err = ESP_OK;
#if DONGLE_ENABLE_USB_MOUSE
        err = usb_mouse_click(USB_MOUSE_BUTTON_LEFT, DONGLE_KEY_TAP_HOLD_MS);
#endif
        dongle_log_mouse_button_event(class_idx, "mouse_click", USB_MOUSE_BUTTON_LEFT, err);
    } else if (action->type == ACTION_TYPE_MOUSE_RIGHT_TIMED_HOLD) {
        dongle_release_mouse_hold();
        dongle_release_timed_mouse_hold(true, now_us);
        const uint16_t hold_ms = action->cooldown_ms > 0 ?
                                 action->cooldown_ms :
                                 DONGLE_MOUSE_RIGHT_HOLD_MS;
        esp_err_t err = ESP_OK;
#if DONGLE_ENABLE_USB_MOUSE
        err = usb_mouse_set_buttons(USB_MOUSE_BUTTON_RIGHT);
#endif
        dongle_log_mouse_button_event(class_idx, "mouse_hold", USB_MOUSE_BUTTON_RIGHT, err);
        if (err == ESP_OK) {
            s_dongle_mouse_timed_hold_class = (int8_t)class_idx;
            s_dongle_mouse_timed_release_us =
                now_us + ((int64_t)hold_ms * 1000LL);
        }
    } else if (action->type == ACTION_TYPE_MOUSE_MOVE_LEFT) {
        esp_err_t err = ESP_OK;
        const int16_t dx = -DONGLE_HAND_MOUSE_MOVE_DELTA;
        err = dongle_mouse_move_x(dx);
        dongle_log_mouse_move_event(class_idx, dx, err);
    } else if (action->type == ACTION_TYPE_MOUSE_MOVE_RIGHT) {
        esp_err_t err = ESP_OK;
        const int16_t dx = DONGLE_HAND_MOUSE_MOVE_DELTA;
        err = dongle_mouse_move_x(dx);
        dongle_log_mouse_move_event(class_idx, dx, err);
    } else if (action->type == ACTION_TYPE_MOUSE_TURN_LEFT) {
        esp_err_t err = ESP_OK;
        const int16_t dx = -DONGLE_TURN_MOUSE_MOVE_DELTA;
        err = dongle_mouse_move_x(dx);
        dongle_log_mouse_move_event(class_idx, dx, err);
    } else if (action->type == ACTION_TYPE_MOUSE_TURN_RIGHT) {
        esp_err_t err = ESP_OK;
        const int16_t dx = DONGLE_TURN_MOUSE_MOVE_DELTA;
        err = dongle_mouse_move_x(dx);
        dongle_log_mouse_move_event(class_idx, dx, err);
    }
}

static void dongle_send_key_action(uint8_t infer_class, float infer_confidence, int64_t now_us)
{
#if DONGLE_ENABLE_USB_KEYBOARD
    if (!usb_keyboard_is_ready()) {
        s_dongle_hid_ready = false;
        s_dongle_hid_status = "usb_not_ready";
        return;
    }
    s_dongle_hid_ready = true;
#else
    s_dongle_hid_ready = false;
#endif
    dongle_release_timed_mouse_hold(false, now_us);
    dongle_init_edge_armed();
    dongle_action_config_ensure_ready();

    if (dongle_blade_pressed_fresh(now_us)) {
        s_dongle_last_blade_pressed_us = now_us;
    }

    uint8_t raw_class = infer_class;
    const bool raw_class_is_confident =
        raw_class < DONGLE_NUM_CLASSES &&
        infer_confidence >= dongle_effective_min_confidence(raw_class);
    if (!raw_class_is_confident) {
        raw_class = DONGLE_IDLE_CLASS;
        s_dongle_hid_status = "below_threshold";
    }

    uint8_t class_idx = dongle_is_view_move_class(raw_class) ?
                        raw_class :
                        dongle_smooth_class(raw_class);
    if (dongle_should_block_opposite_turn(class_idx, now_us)) {
        class_idx = DONGLE_IDLE_CLASS;
        s_dongle_hid_status = "opposite_turn_blocked";
    }

    if (dongle_is_event_class(class_idx)) {
        dongle_note_event_after_state(class_idx, now_us);
    } else if (dongle_is_state_class(class_idx)) {
        const bool resumed_state = dongle_try_resume_state_after_event(&class_idx, now_us);
        if (!resumed_state && (dongle_is_active_state_class(class_idx) || raw_class_is_confident)) {
            dongle_remember_state_class(class_idx, now_us);
        }
    }
    const bool kept_movement_after_blade =
        dongle_try_keep_movement_after_blade(&class_idx, now_us);

    const dongle_key_action_t *action = &s_dongle_key_actions[class_idx];
    const int8_t jump_movement_class =
        (class_idx == DONGLE_JUMP_CLASS && action->type == ACTION_TYPE_KEY_TAP) ?
        dongle_jump_movement_class(now_us) :
        -1;

    /* Update sustain counters: increment for current class, reset others */
    for (uint8_t i = 0; i < DONGLE_NUM_CLASSES; i++) {
        if (i == class_idx) {
            if (s_dongle_sustain_count[i] < UINT16_MAX) {
                s_dongle_sustain_count[i]++;
            }
        } else {
            s_dongle_sustain_count[i] = 0;
        }
    }

    /* Update edge arming: if we left a class, re-arm it */
    for (uint8_t i = 0; i < DONGLE_NUM_CLASSES; i++) {
        if (i != class_idx && !s_dongle_edge_armed[i]) {
            const dongle_key_action_t *a = &s_dongle_key_actions[i];
            if (a->trigger == TRIGGER_EDGE) {
                int64_t since_fire = now_us - s_dongle_last_fire_us[i];
                if (since_fire >= ((int64_t)a->cooldown_ms * 1000LL)) {
                    s_dongle_edge_armed[i] = true;
                }
            }
        }
    }

    /* Handle NONE (idle) */
    if (action->type == ACTION_TYPE_NONE) {
        if (raw_class == DONGLE_IDLE_CLASS) {
            s_dongle_hid_status = "idle_or_uncertain";
        } else {
            s_dongle_hid_status = "no_action";
        }
        if (s_dongle_keyboard_hold_class >= 0 &&
            dongle_is_movement_hold_class((uint8_t)s_dongle_keyboard_hold_class) &&
            s_dongle_last_jump_move_fire_us > 0) {
            const int64_t since_jump_move_us =
                now_us - s_dongle_last_jump_move_fire_us;
            if (since_jump_move_us >= 0 &&
                since_jump_move_us <
                    ((int64_t)DONGLE_JUMP_MOVE_HOLD_GRACE_MS * 1000LL)) {
                /* Just did a jump-with-movement tap; keep W held through
                 * the landing-phase move_noise frames so the player doesn't
                 * stop walking immediately after landing. */
                dongle_release_mouse_hold();
                dongle_release_timed_mouse_hold(true, now_us);
                return;
            }
        }
        dongle_release_hold_actions();
        return;
    }

    /* Handle HOLD (walk/run) */
    if (action->type == ACTION_TYPE_KEY_HOLD) {
        dongle_release_mouse_hold();
        if (s_dongle_keyboard_hold_class != (int8_t)class_idx) {
            dongle_release_keyboard_hold();
            esp_err_t err = ESP_OK;
#if DONGLE_ENABLE_USB_KEYBOARD
            const uint8_t keycodes[6] = { action->keycode, 0, 0, 0, 0, 0 };
            err = usb_keyboard_press_keys(action->modifier, keycodes);
#endif
            dongle_log_key_event(class_idx, "key_hold", action->modifier, action->keycode, err);
            if (err == ESP_OK) {
                s_dongle_keyboard_hold_class = (int8_t)class_idx;
            }
        } else {
            s_dongle_hid_status = kept_movement_after_blade ?
                                  "blade_move_hold_grace" :
                                  "hold_active";
        }
        return;
    }

    /* Handle mouse hold (aim/charged attack) */
    if (action->type == ACTION_TYPE_MOUSE_HOLD) {
        dongle_release_keyboard_hold();
        dongle_release_timed_mouse_hold(true, now_us);
        if (s_dongle_mouse_hold_class != (int8_t)class_idx) {
            dongle_release_mouse_hold();
            esp_err_t err = ESP_OK;
#if DONGLE_ENABLE_USB_MOUSE
            err = usb_mouse_set_buttons(USB_MOUSE_BUTTON_LEFT);
#endif
            dongle_log_mouse_button_event(class_idx, "mouse_hold", USB_MOUSE_BUTTON_LEFT, err);
            if (err == ESP_OK) {
                s_dongle_mouse_hold_class = (int8_t)class_idx;
            }
        } else {
            s_dongle_hid_status = "hold_active";
        }
        return;
    }

    if (action->type == ACTION_TYPE_MOUSE_RIGHT_TIMED_HOLD) {
        dongle_release_keyboard_hold();
        dongle_release_mouse_hold();
    } else if (dongle_is_mouse_view_action(action)) {
        if (dongle_blade_pressed_fresh(now_us)) {
            s_dongle_hid_status = "blade_turn_override";
            return;
        }
        dongle_release_mouse_hold();
        dongle_release_timed_mouse_hold(true, now_us);
    } else {
        /* Release held actions before tap/move actions */
        if (jump_movement_class >= 0) {
            /* Keep W (and Shift while running) in the same HID report as jump. */
            dongle_release_mouse_hold();
            dongle_release_timed_mouse_hold(true, now_us);
        } else {
            dongle_release_hold_actions();
        }
    }

    /* Handle tap-like actions based on trigger mode */
    bool should_fire = false;

    switch (action->trigger) {
    case TRIGGER_COOLDOWN: {
        int64_t elapsed = now_us - s_dongle_last_fire_us[class_idx];
        if (!dongle_action_sustain_ready(class_idx, action)) {
            s_dongle_hid_status = "sustain_wait";
        } else if (elapsed >= ((int64_t)action->cooldown_ms * 1000LL)) {
            should_fire = true;
        } else {
            s_dongle_hid_status = "cooldown_wait";
        }
        break;
    }
    case TRIGGER_EDGE: {
        if (!s_dongle_edge_armed[class_idx]) {
            s_dongle_hid_status = "edge_wait";
        } else if (dongle_action_sustain_ready(class_idx, action)) {
            should_fire = true;
        } else {
            s_dongle_hid_status = "sustain_wait";
        }
        break;
    }
    case TRIGGER_SUSTAIN: {
        if (dongle_action_sustain_fire_frame(class_idx, action)) {
            should_fire = true;
        } else {
            s_dongle_hid_status = "sustain_wait";
        }
        break;
    }
    case TRIGGER_REPEAT: {
        int64_t elapsed = now_us - s_dongle_last_fire_us[class_idx];
        if (!dongle_action_sustain_ready(class_idx, action)) {
            s_dongle_hid_status = "sustain_wait";
        } else if (elapsed >= ((int64_t)action->cooldown_ms * 1000LL)) {
            should_fire = true;
        } else {
            s_dongle_hid_status = "repeat_wait";
        }
        break;
    }
    case TRIGGER_COUNT:
    default:
        s_dongle_hid_status = "invalid_trigger";
        break;
    }

    if (should_fire) {
        dongle_fire_action(class_idx, action, now_us, jump_movement_class);
        s_dongle_last_fire_us[class_idx] = now_us;
        if (dongle_is_turn_class(class_idx)) {
            s_dongle_last_turn_fire_class = (int8_t)class_idx;
            s_dongle_last_turn_fire_us = now_us;
        }
        if (action->trigger == TRIGGER_EDGE) {
            s_dongle_edge_armed[class_idx] = false;
        }
    }
}
#endif

static bool dongle_build_rf_frame(rf_infer_node_sample_t frame[RF_INFER_NODE_COUNT],
                                  int64_t now_us,
                                  double *out_max_age_ms)
{
    double max_age_ms = 0.0;

    for (uint8_t node_id = 1; node_id <= RF_INFER_NODE_COUNT; node_id++) {
        dongle_latest_node_t *node = dongle_latest_node_for_id(node_id);
        if (node == NULL || !node->valid || node->last_rx_us <= 0) {
            return false;
        }

        const int64_t age_us = now_us - node->last_rx_us;
        if (age_us < 0 || age_us > ((int64_t)DONGLE_RF_MAX_NODE_AGE_MS * 1000LL)) {
            return false;
        }

        const double age_ms = (double)age_us / 1000.0;
        if (age_ms > max_age_ms) {
            max_age_ms = age_ms;
        }

        const m2p_espnow_tracker_packet_t *packet = &node->packet;
        rf_infer_node_sample_t *sample = &frame[node_id - 1U];
        sample->ax = packet->accel_g[0];
        sample->ay = packet->accel_g[1];
        sample->az = packet->accel_g[2];
        sample->gx = packet->gyro_dps[0];
        sample->gy = packet->gyro_dps[1];
        sample->gz = packet->gyro_dps[2];
    }

    if (out_max_age_ms != NULL) {
        *out_max_age_ms = max_age_ms;
    }
    return true;
}

#if DONGLE_ENABLE_USB_TELEMETRY
static float dongle_absf(float value)
{
    return value < 0.0f ? -value : value;
}

static uint8_t dongle_motion_intensity_percent(
    const rf_infer_node_sample_t frame[RF_INFER_NODE_COUNT])
{
    float total_score = 0.0f;
    for (uint8_t node = 0; node < RF_INFER_NODE_COUNT; node++) {
        const float accel_magnitude_squared =
            frame[node].ax * frame[node].ax +
            frame[node].ay * frame[node].ay +
            frame[node].az * frame[node].az;
        const float dynamic_acceleration = dongle_absf(accel_magnitude_squared - 1.0f);
        const float gyro_activity = dongle_absf(frame[node].gx) +
                                    dongle_absf(frame[node].gy) +
                                    dongle_absf(frame[node].gz);
        total_score += dynamic_acceleration * 35.0f + gyro_activity / 15.0f;
    }

    float raw_percent = total_score / (float)RF_INFER_NODE_COUNT;
    if (raw_percent > 100.0f) {
        raw_percent = 100.0f;
    }
    s_dongle_smoothed_motion_intensity =
        s_dongle_smoothed_motion_intensity * 0.75f + raw_percent * 0.25f;
    if (s_dongle_smoothed_motion_intensity < 0.0f) {
        s_dongle_smoothed_motion_intensity = 0.0f;
    } else if (s_dongle_smoothed_motion_intensity > 100.0f) {
        s_dongle_smoothed_motion_intensity = 100.0f;
    }
    return (uint8_t)(s_dongle_smoothed_motion_intensity + 0.5f);
}
#endif

static void dongle_run_rf_inference(int64_t now_us)
{
    rf_infer_node_sample_t frame[RF_INFER_NODE_COUNT] = {0};
    rf_state_infer_node_sample_t state_frame[RF_STATE_INFER_NODE_COUNT] = {0};
    double max_age_ms = 0.0;

    if (!dongle_build_rf_frame(frame, now_us, &max_age_ms)) {
        return;
    }
    for (uint8_t node = 0; node < RF_INFER_NODE_COUNT && node < RF_STATE_INFER_NODE_COUNT; node++) {
        state_frame[node].ax = frame[node].ax;
        state_frame[node].ay = frame[node].ay;
        state_frame[node].az = frame[node].az;
        state_frame[node].gx = frame[node].gx;
        state_frame[node].gy = frame[node].gy;
        state_frame[node].gz = frame[node].gz;
    }

    rf_infer_result_t result = {0};
    rf_state_infer_result_t state_result = {0};
    const int64_t infer_start_us = esp_timer_get_time();
    const bool state_valid = rf_state_infer_push_frame(state_frame, &state_result) && state_result.valid;
    const bool event_valid = rf_infer_push_frame(frame, &result) && result.valid;
    if (!state_valid && !event_valid) {
        return;
    }
    const int64_t infer_elapsed_us = esp_timer_get_time() - infer_start_us;

    uint8_t result_class = DONGLE_IDLE_CLASS;
    float result_confidence = 0.0f;
    const char *result_label = "warming_up";
    uint32_t result_frames = state_valid ? state_result.frame_count : result.frame_count;

    uint8_t state_class = DONGLE_IDLE_CLASS;
    const bool state_mapped = state_valid && dongle_class_from_state_label(state_result.label, &state_class);
    const bool event_result_is_action =
        event_valid &&
        result.class_index < DONGLE_NUM_CLASSES &&
        !dongle_is_state_class(result.class_index);

    if (event_result_is_action) {
        result_class = result.class_index;
        result_confidence = result.confidence;
        result_label = result.label;
        result_frames = result.frame_count;
    } else if (state_mapped) {
        result_class = state_class;
        result_confidence = state_result.confidence;
        result_label = state_result.label;
        result_frames = state_result.frame_count;
    } else if (event_valid) {
        result_class = result.class_index;
        result_confidence = result.confidence;
        result_label = result.label;
        result_frames = result.frame_count;
    }

#if DONGLE_ENABLE_USB_TELEMETRY
    dongle_telemetry_update_inference(result_class,
                                      result_confidence,
                                      dongle_motion_intensity_percent(frame),
                                      now_us);
#endif

#if DONGLE_ENABLE_HID_ACTION_LOG
    dongle_send_key_action(result_class, result_confidence, now_us);
#endif

#if !DONGLE_ENABLE_ACTION_DEBUG_OUTPUT
    (void)result_label;
    (void)result_frames;
    (void)infer_elapsed_us;
    return;
#endif

#if DONGLE_ENABLE_ACTION_DEBUG_OUTPUT
    if ((now_us - s_dongle_last_rf_print_us) <
        ((int64_t)DONGLE_RF_PRINT_INTERVAL_MS * 1000LL)) {
        return;
    }
    s_dongle_last_rf_print_us = now_us;

    const char *display_label = result_label;
    const float display_min_confidence = (result_class < DONGLE_NUM_CLASSES) ?
                                         dongle_effective_min_confidence(result_class) :
                                         DONGLE_RF_MIN_CONFIDENCE;
    if (result_confidence < display_min_confidence) {
        display_label = "uncertain";
    }

    const int hid_trigger = s_dongle_hid_trigger_since_last_print ? 1 : 0;
    const uint32_t hid_count = s_dongle_hid_trigger_count_since_last_print;
    const char *hid_action = s_dongle_hid_trigger_since_last_print ?
                             dongle_action_class_name(s_dongle_last_hid_trigger_class) : "none";
    const char *hid_event = s_dongle_hid_trigger_since_last_print ?
                            s_dongle_last_hid_trigger_event : "none";
    const double blade_age_ms = dongle_blade_age_ms(now_us);
    const bool blade_valid = s_dongle_blade_state.valid;
    const bool blade_fresh = dongle_blade_state_fresh(now_us);
    printf("# infer: action=%s conf=%.2f frames=%" PRIu32
           " hid_trigger=%d hid_count=%" PRIu32 " hid_ready=%d hid_status=%s"
           " hid_action=%s hid_event=%s"
           " event_action=%s event_conf=%.2f state_action=%s state_conf=%.2f"
           " max_age_ms=%.1f infer_us=%" PRId64
           " blade_valid=%d blade_pressed=%d blade_fresh=%d"
           " blade_seq=%" PRIu32 " blade_age_ms=%.1f\n",
           display_label,
           (double)result_confidence,
           result_frames,
           hid_trigger,
           hid_count,
           s_dongle_hid_ready ? 1 : 0,
           s_dongle_hid_status,
           hid_action,
           hid_event,
           event_valid ? result.label : "warming_up",
           event_valid ? (double)result.confidence : 0.0,
           state_valid ? state_result.label : "warming_up",
           state_valid ? (double)state_result.confidence : 0.0,
           max_age_ms,
           infer_elapsed_us,
           blade_valid ? 1 : 0,
           (blade_valid && s_dongle_blade_state.pressed) ? 1 : 0,
           blade_fresh ? 1 : 0,
           blade_valid ? s_dongle_blade_state.sequence : 0,
           blade_age_ms);
    s_dongle_hid_trigger_since_last_print = false;
    s_dongle_hid_trigger_count_since_last_print = 0;
    s_dongle_last_hid_trigger_class = DONGLE_IDLE_CLASS;
    s_dongle_last_hid_trigger_event = "none";
#endif
}
#endif

typedef struct {
    bool seen;
    bool online;
    bool battery_valid;
    uint8_t node_id;
    const char *name;
    uint32_t sequence;
    uint32_t age_ms;
    uint32_t battery_age_ms;
    uint16_t battery_mv;
    uint8_t battery_percent;
} dongle_status_node_snapshot_t;

typedef struct {
    bool seen;
    bool online;
    bool pressed;
    bool battery_valid;
    uint8_t node_id;
    uint32_t sequence;
    uint32_t age_ms;
    uint32_t battery_age_ms;
    uint16_t battery_mv;
    uint8_t battery_percent;
} dongle_status_blade_snapshot_t;

static httpd_handle_t s_dongle_status_httpd;
static volatile bool s_dongle_wifi_display_active;

static uint32_t dongle_age_ms(int64_t now_us, int64_t last_us)
{
    if (last_us <= 0 || now_us < last_us) {
        return UINT32_MAX;
    }

    const int64_t age_ms = (now_us - last_us) / 1000LL;
    if (age_ms > UINT32_MAX) {
        return UINT32_MAX;
    }

    return (uint32_t)age_ms;
}

static void dongle_status_snapshot(dongle_status_node_snapshot_t snapshot[DONGLE_MAX_TRACKER_NODES],
                                   dongle_status_blade_snapshot_t *blade)
{
    const int64_t now_us = esp_timer_get_time();

    dongle_state_lock();
    for (uint8_t i = 0; i < DONGLE_MAX_TRACKER_NODES; i++) {
        const dongle_latest_node_t *node = &s_dongle_latest_nodes[i];
        dongle_status_node_snapshot_t *item = &snapshot[i];
        const uint8_t node_id = (uint8_t)(i + 1U);
        const uint32_t age_ms = dongle_age_ms(now_us, node->last_rx_us);
        const uint32_t battery_age_ms = dongle_age_ms(now_us, node->last_battery_rx_us);

        item->seen = node->valid;
        item->online = node->valid && age_ms <= DONGLE_STATUS_ONLINE_MAX_AGE_MS;
        item->battery_valid = node->battery_valid;
        item->node_id = node_id;
        item->name = tracker_node_name(node_id);
        item->sequence = node->valid ? node->packet.sequence : 0;
        item->age_ms = age_ms;
        item->battery_age_ms = battery_age_ms;
        item->battery_mv = node->battery_mv;
        item->battery_percent = node->battery_percent;
    }

    if (blade != NULL) {
        const uint32_t age_ms = dongle_age_ms(now_us, s_dongle_blade_state.last_rx_us);
        const uint32_t battery_age_ms =
            dongle_age_ms(now_us, s_dongle_blade_state.last_battery_rx_us);
        blade->seen = s_dongle_blade_state.valid;
        blade->online = s_dongle_blade_state.valid && age_ms <= DONGLE_STATUS_ONLINE_MAX_AGE_MS;
        blade->pressed = s_dongle_blade_state.valid && s_dongle_blade_state.pressed;
        blade->battery_valid = s_dongle_blade_state.battery_valid;
        blade->node_id = s_dongle_blade_state.valid ? s_dongle_blade_state.node_id : BLADE_NODE_ID;
        blade->sequence = s_dongle_blade_state.valid ? s_dongle_blade_state.sequence : 0;
        blade->age_ms = age_ms;
        blade->battery_age_ms = battery_age_ms;
        blade->battery_mv = s_dongle_blade_state.battery_mv;
        blade->battery_percent = s_dongle_blade_state.battery_percent;
    }
    dongle_state_unlock();
}

#if DONGLE_ENABLE_USB_TELEMETRY
static int dongle_telemetry_battery_value(const dongle_status_node_snapshot_t *node)
{
    return node->battery_valid ? (int)node->battery_percent : -1;
}

static void dongle_telemetry_maybe_send(int64_t now_us)
{
    if (!usb_telemetry_is_ready() ||
        now_us - s_dongle_last_telemetry_send_us <
            ((int64_t)DONGLE_TELEMETRY_PERIOD_MS * 1000LL)) {
        return;
    }
    s_dongle_last_telemetry_send_us = now_us;

    dongle_status_node_snapshot_t nodes[DONGLE_MAX_TRACKER_NODES] = {0};
    dongle_status_blade_snapshot_t blade = {0};
    dongle_status_snapshot(nodes, &blade);

    uint8_t tracker_mask = 0;
    uint8_t tracker_online = 0;
    for (uint8_t i = 0; i < RF_INFER_NODE_COUNT; i++) {
        if (nodes[i].online) {
            tracker_mask |= (uint8_t)(1U << i);
            tracker_online++;
        }
    }

    uint8_t action_class = s_dongle_telemetry_state.action_class;
    uint8_t confidence_percent = s_dongle_telemetry_state.confidence_percent;
    uint8_t intensity_percent = s_dongle_telemetry_state.intensity_percent;
    bool active = s_dongle_telemetry_state.active;
    if (s_dongle_telemetry_state.last_inference_us <= 0 ||
        now_us - s_dongle_telemetry_state.last_inference_us >
            ((int64_t)DONGLE_TELEMETRY_STALE_MS * 1000LL)) {
        action_class = DONGLE_IDLE_CLASS;
        confidence_percent = 0;
        intensity_percent = 0;
        active = false;
    }

    const uint8_t quality_percent =
        (uint8_t)((tracker_online * 100U) / RF_INFER_NODE_COUNT);
    const int blade_battery = blade.battery_valid ? (int)blade.battery_percent : -1;
    char line[500];
    const int written = snprintf(
        line,
        sizeof(line),
        "{\"v\":1,\"source\":\"MoveToPlay-Dongle\",\"seq\":%" PRIu32
        ",\"time_ms\":%" PRIu64 ",\"type\":\"state\",\"action_id\":%u"
        ",\"action\":\"%s\",\"confidence\":%u,\"intensity\":%u,\"active\":%s"
        ",\"event_count\":%" PRIu32 ",\"event_action\":\"%s\",\"event\":\"%s\""
        ",\"tracker_mask\":%u,\"tracker_online\":%u,\"quality\":%u"
        ",\"blade_online\":%s,\"battery\":[%d,%d,%d,%d,%d]}\n",
        s_dongle_telemetry_packet_sequence++,
        (uint64_t)(now_us / 1000LL),
        action_class,
        dongle_action_class_name(action_class),
        confidence_percent,
        intensity_percent,
        active ? "true" : "false",
        s_dongle_telemetry_state.event_count,
        dongle_action_class_name(s_dongle_telemetry_state.event_action_class),
        s_dongle_telemetry_state.event_name,
        tracker_mask,
        tracker_online,
        quality_percent,
        blade.online ? "true" : "false",
        dongle_telemetry_battery_value(&nodes[0]),
        dongle_telemetry_battery_value(&nodes[1]),
        dongle_telemetry_battery_value(&nodes[2]),
        dongle_telemetry_battery_value(&nodes[3]),
        blade_battery);

    if (written <= 0 || written >= (int)sizeof(line)) {
        ESP_LOGW(TAG, "USB telemetry packet is too large");
        return;
    }
    (void)usb_telemetry_write_line(line);
}
#endif

static const char s_dongle_status_index_prefix[] =
"<!doctype html><html><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>MoveToPlay 接收器</title>"
"<style>"
":root{font-family:system-ui,-apple-system,Segoe UI,sans-serif;color:#17202a;background:#f5f7f9}"
"body{margin:0;padding:20px}"
"main{max-width:1120px;margin:0 auto}"
"h1{margin:0 0 6px;font-size:28px;font-weight:700}"
"h2{margin:28px 0 10px;font-size:20px}"
".sub{margin:0 0 18px;color:#5d6875}"
".panel{margin-top:16px}"
".scroll{overflow-x:auto;border:1px solid #d9e0e7;border-radius:8px;background:#fff}"
"table{width:100%;border-collapse:collapse;background:#fff}"
"th,td{padding:10px 12px;text-align:left;border-bottom:1px solid #e8edf2;font-size:14px;white-space:nowrap}"
"th{background:#eef3f7;color:#3b4652;font-size:12px;text-transform:uppercase}"
"tr:last-child td{border-bottom:0}"
".config th,.config td{padding:8px}"
"select,input{font:inherit;box-sizing:border-box;min-height:34px;border:1px solid #cbd5df;border-radius:6px;background:#fff;color:#17202a}"
"select{min-width:112px;padding:4px 8px}"
"input{width:78px;padding:4px 8px}"
"button{min-height:36px;border:1px solid #1f5f8b;border-radius:6px;background:#1f78b4;color:#fff;font:inherit;font-weight:600;padding:6px 12px}"
"button.secondary{border-color:#a7b0ba;background:#fff;color:#17202a}"
".actions{display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin-top:12px}"
".dot{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:8px;background:#a7b0ba}"
".dot.on{background:#1f9d55}.dot.off{background:#c33d3d}"
".pill{display:inline-block;min-width:64px;padding:3px 8px;border-radius:999px;background:#edf2f7;color:#334155;text-align:center}"
".age{color:#5d6875}"
".muted{color:#5d6875}"
".err{color:#b42318}"
"@media(max-width:640px){body{padding:12px}th,td{padding:8px;font-size:13px}h1{font-size:24px}select{min-width:100px}input{width:70px}}"
"</style></head><body><main>"
"<h1>MoveToPlay 接收器</h1>"
"<p class=\"sub\">连接 Wi-Fi：" DONGLE_WIFI_DISPLAY_AP_SSID "，然后打开 http://192.168.4.1/。</p>"
"<section class=\"panel\"><h2>Blade 状态</h2>"
"<div class=\"scroll\"><table><thead><tr><th>节点</th><th>状态</th><th>按键</th><th>电量</th><th>最后数据</th><th>序号</th></tr></thead>"
"<tbody id=\"blade\"><tr><td colspan=\"6\" class=\"muted\">加载中...</td></tr></tbody></table></div></section>"
"<section class=\"panel\"><h2>Tracker 状态</h2>"
"<div class=\"scroll\"><table><thead><tr><th>ID</th><th>节点</th><th>状态</th><th>电量</th><th>最后数据</th></tr></thead>"
"<tbody id=\"nodes\"><tr><td colspan=\"5\" class=\"muted\">加载中...</td></tr></tbody></table></div>"
"<p id=\"statusMsg\" class=\"sub\"></p></section>"
"<section class=\"panel\"><h2>按键配置</h2>";

static const char s_dongle_status_index_suffix[] =
"</section>"
"<script>"
"function esc(v){return String(v).replace(/[&<>]/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;'}[c];});}"
"function battery(n){return n.battery_known?n.battery_percent+'% ('+(n.battery_mv/1000).toFixed(2)+'V)':'未知';}"
"function age(n){return n.seen?n.age_ms+' ms':'从未收到';}"
"function bladeRow(b){const on=b&&b.online;const seen=b&&b.seen;const pressed=seen&&b.pressed;return '<tr><td>Blade '+(b?b.id:'')+'</td><td><span class=\"dot '+(on?'on':'off')+'\"></span>'+(on?'在线':'离线')+'</td><td>'+(pressed?'按下':'未按下')+'</td><td>'+battery(b||{})+'</td><td class=\"age\">'+(seen?b.age_ms+' ms':'从未收到')+'</td><td>'+(seen?b.sequence:'-')+'</td></tr>';}"
"async function loadStatus(){try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw new Error(r.status);const d=await r.json();let html='';d.nodes.forEach(function(n){const on=n.online;html+='<tr><td>'+n.id+'</td><td>'+esc(n.name)+'</td><td><span class=\"dot '+(on?'on':'off')+'\"></span>'+(on?'在线':'离线')+'</td><td>'+battery(n)+'</td><td class=\"age\">'+age(n)+'</td></tr>';});document.getElementById('nodes').innerHTML=html;document.getElementById('blade').innerHTML=bladeRow(d.blade);document.getElementById('statusMsg').textContent='运行时间 '+Math.floor(d.uptime_ms/1000)+' 秒';}"
"catch(e){document.getElementById('statusMsg').textContent='状态更新失败';document.getElementById('statusMsg').className='sub err';}}"
"loadStatus();setInterval(loadStatus,1000);"
"</script>"
"</main></body></html>";

#if DONGLE_ENABLE_RF_INFERENCE && DONGLE_ENABLE_HID_ACTION_LOG
static esp_err_t dongle_http_send_select_options(httpd_req_t *req,
                                                 const dongle_value_label_t *options,
                                                 size_t option_count,
                                                 uint8_t selected)
{
    char chunk[128];

    for (size_t i = 0; i < option_count; i++) {
        snprintf(chunk,
                 sizeof(chunk),
                 "<option value=\"%u\"%s>%s</option>",
                 options[i].value,
                 (options[i].value == selected) ? " selected" : "",
                 options[i].label);
        esp_err_t err = httpd_resp_sendstr_chunk(req, chunk);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

static esp_err_t dongle_http_send_config_form(httpd_req_t *req)
{
    char chunk[384];

    dongle_action_config_ensure_ready();

    esp_err_t err = httpd_resp_sendstr_chunk(
        req,
        "<p class=\"sub\">保存后配置会写入接收器 flash，重启或退出 Wi-Fi 显示模式后继续生效。</p>"
        "<form method=\"post\" action=\"/api/config\">"
        "<div class=\"scroll\"><table class=\"config\"><thead><tr>"
        "<th>ID</th><th>动作</th><th>输出</th><th>按键</th><th>组合键</th>"
        "<th>触发</th><th>置信度 %</th><th>冷却 ms</th><th>持续帧</th>"
        "</tr></thead><tbody>");
    if (err != ESP_OK) {
        return err;
    }

    for (uint8_t i = 0; i < DONGLE_NUM_CLASSES; i++) {
        const dongle_key_action_t *action = &s_dongle_key_actions[i];

        snprintf(chunk,
                 sizeof(chunk),
                 "<tr><td>%u</td><td>%s<br><span class=\"muted\">%s</span></td>"
                 "<td><select name=\"c%u_type\">",
                 i,
                 dongle_action_class_display_name(i),
                 dongle_action_class_name(i),
                 i);
        err = httpd_resp_sendstr_chunk(req, chunk);
        if (err != ESP_OK) {
            return err;
        }
        err = dongle_http_send_select_options(req,
                                              s_action_type_options,
                                              DONGLE_ARRAY_SIZE(s_action_type_options),
                                              (uint8_t)action->type);
        if (err != ESP_OK) {
            return err;
        }

        snprintf(chunk, sizeof(chunk), "</select></td><td><select name=\"c%u_key\">", i);
        err = httpd_resp_sendstr_chunk(req, chunk);
        if (err != ESP_OK) {
            return err;
        }
        err = dongle_http_send_select_options(req,
                                              s_key_options,
                                              DONGLE_ARRAY_SIZE(s_key_options),
                                              action->keycode);
        if (err != ESP_OK) {
            return err;
        }

        snprintf(chunk, sizeof(chunk), "</select></td><td><select name=\"c%u_mod\">", i);
        err = httpd_resp_sendstr_chunk(req, chunk);
        if (err != ESP_OK) {
            return err;
        }
        err = dongle_http_send_select_options(req,
                                              s_modifier_options,
                                              DONGLE_ARRAY_SIZE(s_modifier_options),
                                              action->modifier);
        if (err != ESP_OK) {
            return err;
        }

        snprintf(chunk, sizeof(chunk), "</select></td><td><select name=\"c%u_trig\">", i);
        err = httpd_resp_sendstr_chunk(req, chunk);
        if (err != ESP_OK) {
            return err;
        }
        err = dongle_http_send_select_options(req,
                                              s_trigger_options,
                                              DONGLE_ARRAY_SIZE(s_trigger_options),
                                              (uint8_t)action->trigger);
        if (err != ESP_OK) {
            return err;
        }

        snprintf(chunk,
                 sizeof(chunk),
                 "</select></td>"
                 "<td><input name=\"c%u_conf\" type=\"number\" min=\"%u\" max=\"%u\" value=\"%u\"></td>"
                 "<td><input name=\"c%u_cool\" type=\"number\" min=\"0\" max=\"%u\" value=\"%u\"></td>"
                 "<td><input name=\"c%u_sus\" type=\"number\" min=\"0\" max=\"%u\" value=\"%u\"></td></tr>",
                 i,
                 DONGLE_RF_MIN_CONFIDENCE_PERCENT,
                 DONGLE_RF_MAX_CONFIDENCE_PERCENT,
                 s_dongle_action_confidence_percent[i],
                 i,
                 DONGLE_MAX_COOLDOWN_MS,
                 action->cooldown_ms,
                 i,
                 DONGLE_MAX_SUSTAIN_FRAMES,
                 action->sustain_frames);
        err = httpd_resp_sendstr_chunk(req, chunk);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = httpd_resp_sendstr_chunk(
        req,
        "</tbody></table></div>"
        "<div class=\"actions\"><button type=\"submit\">保存配置</button></div>"
        "</form>"
        "<form method=\"post\" action=\"/api/config/reset\" class=\"actions\">"
        "<button class=\"secondary\" type=\"submit\">恢复默认</button>"
        "</form>"
        "<p class=\"sub\">API: /api/status, /api/config</p>");
    return err;
}
#endif

static esp_err_t dongle_status_index_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "HTTP request: GET /");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    esp_err_t err = httpd_resp_sendstr_chunk(req, s_dongle_status_index_prefix);
    if (err != ESP_OK) {
        return err;
    }

#if DONGLE_ENABLE_RF_INFERENCE && DONGLE_ENABLE_HID_ACTION_LOG
    err = dongle_http_send_config_form(req);
#else
    err = httpd_resp_sendstr_chunk(req, "<p class=\"sub\">当前固件模式没有启用按键配置。</p>");
#endif
    if (err != ESP_OK) {
        return err;
    }

    err = httpd_resp_sendstr_chunk(req, s_dongle_status_index_suffix);
    if (err != ESP_OK) {
        return err;
    }

    err = httpd_resp_sendstr_chunk(req, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP response failed: / %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "HTTP response: / OK");
    return ESP_OK;
}

static esp_err_t dongle_status_api_get_handler(httpd_req_t *req)
{
    dongle_status_node_snapshot_t snapshot[DONGLE_MAX_TRACKER_NODES] = {0};
    dongle_status_blade_snapshot_t blade = {0};
    char chunk[384];

    ESP_LOGI(TAG, "HTTP request: GET /api/status");
    dongle_status_snapshot(snapshot, &blade);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    snprintf(chunk,
             sizeof(chunk),
             "{\"mode\":\"wifi_display\",\"uptime_ms\":%" PRIu32 ",\"online_max_age_ms\":%u,\"nodes\":[",
             (uint32_t)(esp_timer_get_time() / 1000LL),
             (unsigned)DONGLE_STATUS_ONLINE_MAX_AGE_MS);
    esp_err_t err = httpd_resp_sendstr_chunk(req, chunk);
    if (err != ESP_OK) {
        return err;
    }

    for (uint8_t i = 0; i < DONGLE_MAX_TRACKER_NODES; i++) {
        const dongle_status_node_snapshot_t *node = &snapshot[i];
        const uint32_t age_ms = (node->age_ms == UINT32_MAX) ? 0 : node->age_ms;
        const uint32_t battery_age_ms = (node->battery_age_ms == UINT32_MAX) ? 0 : node->battery_age_ms;

        snprintf(chunk,
                 sizeof(chunk),
                 "%s{\"id\":%u,\"name\":\"%s\",\"seen\":%s,\"online\":%s,"
                 "\"age_ms\":%" PRIu32 ",\"sequence\":%" PRIu32 ","
                 "\"battery_known\":%s,\"battery_percent\":%u,"
                 "\"battery_mv\":%u,\"battery_age_ms\":%" PRIu32 "}",
                 (i == 0) ? "" : ",",
                 node->node_id,
                 node->name,
                 node->seen ? "true" : "false",
                 node->online ? "true" : "false",
                 age_ms,
                  node->sequence,
                  node->battery_valid ? "true" : "false",
                  node->battery_valid ? node->battery_percent : 0,
                  node->battery_valid ? node->battery_mv : 0,
                  battery_age_ms);
        err = httpd_resp_sendstr_chunk(req, chunk);
        if (err != ESP_OK) {
            return err;
        }
    }

    const uint32_t blade_age_ms = (blade.age_ms == UINT32_MAX) ? 0 : blade.age_ms;
    const uint32_t blade_battery_age_ms =
        (blade.battery_age_ms == UINT32_MAX) ? 0 : blade.battery_age_ms;
    snprintf(chunk,
             sizeof(chunk),
             "],\"blade\":{\"id\":%u,\"name\":\"blade\",\"seen\":%s,\"online\":%s,"
             "\"pressed\":%s,\"age_ms\":%" PRIu32 ",\"sequence\":%" PRIu32 ","
             "\"battery_known\":%s,\"battery_percent\":%u,"
             "\"battery_mv\":%u,\"battery_age_ms\":%" PRIu32 "}}",
             blade.node_id,
             blade.seen ? "true" : "false",
             blade.online ? "true" : "false",
             blade.pressed ? "true" : "false",
             blade_age_ms,
             blade.sequence,
             blade.battery_valid ? "true" : "false",
             blade.battery_valid ? blade.battery_percent : 0,
             blade.battery_valid ? blade.battery_mv : 0,
             blade_battery_age_ms);
    err = httpd_resp_sendstr_chunk(req, chunk);
    if (err != ESP_OK) {
        return err;
    }

    err = httpd_resp_sendstr_chunk(req, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP response failed: /api/status %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "HTTP response: /api/status OK");
    return ESP_OK;
}

#if DONGLE_ENABLE_RF_INFERENCE && DONGLE_ENABLE_HID_ACTION_LOG
static esp_err_t dongle_config_api_get_handler(httpd_req_t *req)
{
    char chunk[384];

    ESP_LOGI(TAG, "HTTP request: GET /api/config");
    dongle_action_config_ensure_ready();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    esp_err_t err = httpd_resp_sendstr_chunk(req,
                                             "{\"version\":1,\"confidence_min\":30,"
                                             "\"confidence_max\":95,\"classes\":[");
    if (err != ESP_OK) {
        return err;
    }

    for (uint8_t i = 0; i < DONGLE_NUM_CLASSES; i++) {
        const dongle_key_action_t *action = &s_dongle_key_actions[i];
        snprintf(chunk,
                 sizeof(chunk),
                 "%s{\"id\":%u,\"name\":\"%s\",\"display_name\":\"%s\","
                 "\"type\":%u,\"type_name\":\"%s\","
                 "\"keycode\":%u,\"key\":\"%s\",\"modifier\":%u,"
                 "\"trigger\":%u,\"trigger_name\":\"%s\","
                 "\"confidence_percent\":%u,\"cooldown_ms\":%u,\"sustain_frames\":%u}",
                 (i == 0) ? "" : ",",
                 i,
                 dongle_action_class_name(i),
                 dongle_action_class_display_name(i),
                 (unsigned)action->type,
                 dongle_action_type_name((uint8_t)action->type),
                 action->keycode,
                 dongle_key_name(action->keycode),
                 action->modifier,
                 (unsigned)action->trigger,
                 dongle_trigger_name((uint8_t)action->trigger),
                 s_dongle_action_confidence_percent[i],
                 action->cooldown_ms,
                 action->sustain_frames);
        err = httpd_resp_sendstr_chunk(req, chunk);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = httpd_resp_sendstr_chunk(req, "]}");
    if (err != ESP_OK) {
        return err;
    }

    err = httpd_resp_sendstr_chunk(req, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP response failed: /api/config %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "HTTP response: /api/config OK");
    return ESP_OK;
}

static esp_err_t dongle_http_redirect_root(httpd_req_t *req)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t dongle_http_receive_body(httpd_req_t *req, char **out_body)
{
    if (req->content_len > DONGLE_ACTION_CONFIG_POST_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "config body too large");
        return ESP_FAIL;
    }

    char *body = (char *)malloc(req->content_len + 1U);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_ERR_NO_MEM;
    }

    int received = 0;
    while (received < req->content_len) {
        const int ret = httpd_req_recv(req,
                                       body + received,
                                       req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "receive failed");
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';
    *out_body = body;
    return ESP_OK;
}

static bool dongle_form_get_u32(const char *body, const char *name, uint32_t *out_value)
{
    const size_t name_len = strlen(name);
    const char *p = body;

    while (p != NULL && *p != '\0') {
        const char *amp = strchr(p, '&');
        const size_t segment_len = (amp != NULL) ? (size_t)(amp - p) : strlen(p);

        if (segment_len > (name_len + 1U) &&
            strncmp(p, name, name_len) == 0 &&
            p[name_len] == '=') {
            char value_buf[16];
            const size_t value_len = segment_len - name_len - 1U;
            const size_t copy_len = (value_len < (sizeof(value_buf) - 1U)) ?
                                    value_len :
                                    (sizeof(value_buf) - 1U);
            memcpy(value_buf, p + name_len + 1U, copy_len);
            value_buf[copy_len] = '\0';

            char *end = NULL;
            const unsigned long value = strtoul(value_buf, &end, 10);
            if (end == value_buf) {
                return false;
            }
            *out_value = (uint32_t)value;
            return true;
        }

        p = (amp != NULL) ? (amp + 1) : NULL;
    }

    return false;
}

static void dongle_config_update_from_form(const char *body)
{
    dongle_action_config_storage_t storage;
    char name[24];
    uint32_t value = 0;

    dongle_action_config_export(&storage);

    for (uint8_t i = 0; i < DONGLE_NUM_CLASSES; i++) {
        dongle_action_config_entry_t *entry = &storage.entries[i];

        snprintf(name, sizeof(name), "c%u_type", i);
        if (dongle_form_get_u32(body, name, &value) && value < ACTION_TYPE_COUNT) {
            entry->type = (uint8_t)value;
        }

        snprintf(name, sizeof(name), "c%u_key", i);
        if (dongle_form_get_u32(body, name, &value) &&
            value <= UINT8_MAX &&
            dongle_option_value_exists(s_key_options, DONGLE_ARRAY_SIZE(s_key_options), (uint8_t)value)) {
            entry->keycode = (uint8_t)value;
        }

        snprintf(name, sizeof(name), "c%u_mod", i);
        if (dongle_form_get_u32(body, name, &value) &&
            value <= UINT8_MAX &&
            dongle_option_value_exists(s_modifier_options, DONGLE_ARRAY_SIZE(s_modifier_options), (uint8_t)value)) {
            entry->modifier = (uint8_t)value;
        }

        snprintf(name, sizeof(name), "c%u_trig", i);
        if (dongle_form_get_u32(body, name, &value) && value < TRIGGER_COUNT) {
            entry->trigger = (uint8_t)value;
        }

        snprintf(name, sizeof(name), "c%u_conf", i);
        if (dongle_form_get_u32(body, name, &value)) {
            entry->confidence_percent = dongle_clamp_u8(value,
                                                        DONGLE_RF_MIN_CONFIDENCE_PERCENT,
                                                        DONGLE_RF_MAX_CONFIDENCE_PERCENT);
        }

        snprintf(name, sizeof(name), "c%u_cool", i);
        if (dongle_form_get_u32(body, name, &value)) {
            entry->cooldown_ms = dongle_clamp_u16(value, 0, DONGLE_MAX_COOLDOWN_MS);
        }

        snprintf(name, sizeof(name), "c%u_sus", i);
        if (dongle_form_get_u32(body, name, &value)) {
            entry->sustain_frames = dongle_clamp_u16(value, 0, DONGLE_MAX_SUSTAIN_FRAMES);
        }
    }

    dongle_action_config_apply(&storage);
}

static esp_err_t dongle_config_api_post_handler(httpd_req_t *req)
{
    char *body = NULL;

    ESP_LOGI(TAG, "HTTP request: POST /api/config");
    esp_err_t err = dongle_http_receive_body(req, &body);
    if (err != ESP_OK) {
        return err;
    }

    dongle_config_update_from_form(body);
    free(body);

    err = dongle_action_config_save_current();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save dongle action config failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return err;
    }

    ESP_LOGI(TAG, "dongle action config saved");
    return dongle_http_redirect_root(req);
}

static esp_err_t dongle_config_reset_api_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "HTTP request: POST /api/config/reset");
    esp_err_t err = dongle_action_config_reset_defaults();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reset dongle action config failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "reset failed");
        return err;
    }

    ESP_LOGI(TAG, "dongle action config reset to defaults");
    return dongle_http_redirect_root(req);
}
#endif

static esp_err_t dongle_status_http_start(void)
{
    if (s_dongle_status_httpd != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_open_sockets = 4;
    config.max_uri_handlers = 5;
    config.stack_size = 6144;
    config.task_priority = 8;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;

    esp_err_t err = httpd_start(&s_dongle_status_httpd, &config);
    if (err != ESP_OK) {
        s_dongle_status_httpd = NULL;
        return err;
    }

    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = dongle_status_index_get_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_dongle_status_httpd, &index_uri);
    if (err != ESP_OK) {
        httpd_stop(s_dongle_status_httpd);
        s_dongle_status_httpd = NULL;
        return err;
    }

    const httpd_uri_t api_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = dongle_status_api_get_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_dongle_status_httpd, &api_uri);
    if (err != ESP_OK) {
        httpd_stop(s_dongle_status_httpd);
        s_dongle_status_httpd = NULL;
        return err;
    }

#if DONGLE_ENABLE_RF_INFERENCE && DONGLE_ENABLE_HID_ACTION_LOG
    const httpd_uri_t config_get_uri = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = dongle_config_api_get_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_dongle_status_httpd, &config_get_uri);
    if (err != ESP_OK) {
        httpd_stop(s_dongle_status_httpd);
        s_dongle_status_httpd = NULL;
        return err;
    }

    const httpd_uri_t config_post_uri = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = dongle_config_api_post_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_dongle_status_httpd, &config_post_uri);
    if (err != ESP_OK) {
        httpd_stop(s_dongle_status_httpd);
        s_dongle_status_httpd = NULL;
        return err;
    }

    const httpd_uri_t config_reset_uri = {
        .uri = "/api/config/reset",
        .method = HTTP_POST,
        .handler = dongle_config_reset_api_post_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_dongle_status_httpd, &config_reset_uri);
    if (err != ESP_OK) {
        httpd_stop(s_dongle_status_httpd);
        s_dongle_status_httpd = NULL;
        return err;
    }
#endif

    ESP_LOGI(TAG, "dongle status HTTP server started on port 80");
    return ESP_OK;
}

static esp_err_t dongle_wifi_display_start(void)
{
    if (s_dongle_wifi_display_active) {
        return ESP_OK;
    }

    esp_err_t err = m2p_espnow_enable_softap(DONGLE_WIFI_DISPLAY_AP_SSID,
                                             DONGLE_WIFI_DISPLAY_AP_PASS,
                                             M2P_ESPNOW_CHANNEL,
                                             DONGLE_WIFI_DISPLAY_AP_MAX_CONN);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SoftAP start failed: %s", esp_err_to_name(err));
        return err;
    }

    err = dongle_status_http_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP status server start failed: %s", esp_err_to_name(err));
        return err;
    }

#if DONGLE_ENABLE_USB_KEYBOARD || DONGLE_ENABLE_USB_MOUSE
    if (s_dongle_hid_mutex != NULL) {
        (void)xSemaphoreTake(s_dongle_hid_mutex, portMAX_DELAY);
    }
#endif

    s_dongle_wifi_display_active = true;

#if DONGLE_ENABLE_RF_INFERENCE && DONGLE_ENABLE_HID_ACTION_LOG
    dongle_release_hold_actions();
#endif

#if DONGLE_ENABLE_USB_KEYBOARD || DONGLE_ENABLE_USB_MOUSE
    err = usb_keyboard_switch_to_serial_jtag();
    if (s_dongle_hid_mutex != NULL) {
        (void)xSemaphoreGive(s_dongle_hid_mutex);
    }
    if (err != ESP_OK) {
        s_dongle_wifi_display_active = false;
        ESP_LOGW(TAG, "USB maintenance mode switch failed: %s", esp_err_to_name(err));
        return err;
    }
#endif

    status_led_set_color(0, 0, 20);
    ESP_LOGI(TAG,
             "Wi-Fi display mode ready: ssid=%s url=http://192.168.4.1/",
             DONGLE_WIFI_DISPLAY_AP_SSID);
    ESP_LOGI(TAG, "USB HID disabled; hardware USB Serial/JTAG COM port is active for flashing");
    ESP_LOGI(TAG,
             "Wi-Fi display mode throttles ESP-NOW status updates to %d ms/node and pauses RF/HID processing",
             DONGLE_WIFI_DISPLAY_STATUS_UPDATE_MS);
    printf("# dongle-wifi-display: ssid=%s url=http://192.168.4.1/\n",
           DONGLE_WIFI_DISPLAY_AP_SSID);
    return ESP_OK;
}

static esp_err_t dongle_wifi_display_button_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << (uint32_t)DONGLE_WIFI_DISPLAY_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    return gpio_config(&io_conf);
}

static void dongle_wifi_display_button_task(void *arg)
{
    (void)arg;

    esp_err_t err = dongle_wifi_display_button_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GPIO%d Wi-Fi display trigger init failed: %s",
                 DONGLE_WIFI_DISPLAY_GPIO,
                 esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG,
             "Hold GPIO%d at level %d for %d ms to enter Wi-Fi display mode; hold again to restart and exit",
             DONGLE_WIFI_DISPLAY_GPIO,
             DONGLE_WIFI_DISPLAY_ACTIVE_LEVEL,
             DONGLE_WIFI_DISPLAY_HOLD_MS);

    bool pressed = false;
    bool ready_for_press =
        gpio_get_level(DONGLE_WIFI_DISPLAY_GPIO) != DONGLE_WIFI_DISPLAY_ACTIVE_LEVEL;
    TickType_t press_start_tick = 0;

    if (!ready_for_press) {
        ESP_LOGW(TAG,
                 "GPIO%d is already at active level %d at boot; release it before Wi-Fi display trigger is armed",
                 DONGLE_WIFI_DISPLAY_GPIO,
                 DONGLE_WIFI_DISPLAY_ACTIVE_LEVEL);
    }

    while (1) {
        const int gpio_level = gpio_get_level(DONGLE_WIFI_DISPLAY_GPIO);
        const bool now_pressed = gpio_level == DONGLE_WIFI_DISPLAY_ACTIVE_LEVEL;
        const TickType_t now_tick = xTaskGetTickCount();

        if (!ready_for_press) {
            if (!now_pressed) {
                ready_for_press = true;
                ESP_LOGI(TAG,
                         "GPIO%d released to level %d; Wi-Fi display trigger armed",
                         DONGLE_WIFI_DISPLAY_GPIO,
                         gpio_level);
            }
            vTaskDelay(pdMS_TO_TICKS(DONGLE_WIFI_DISPLAY_POLL_MS));
            continue;
        }

        if (now_pressed && !pressed) {
            pressed = true;
            press_start_tick = now_tick;
            ESP_LOGI(TAG, "GPIO%d hold started", DONGLE_WIFI_DISPLAY_GPIO);
        } else if (!now_pressed) {
            if (pressed) {
                ESP_LOGI(TAG, "GPIO%d hold released", DONGLE_WIFI_DISPLAY_GPIO);
            }
            pressed = false;
        }

        if (pressed) {
            const uint32_t held_ms = (uint32_t)((now_tick - press_start_tick) * portTICK_PERIOD_MS);
            if (held_ms >= DONGLE_WIFI_DISPLAY_HOLD_MS) {
                if (s_dongle_wifi_display_active) {
                    ESP_LOGI(TAG, "GPIO%d held for %u ms in Wi-Fi display mode; restarting to exit",
                             DONGLE_WIFI_DISPLAY_GPIO,
                             (unsigned)held_ms);
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_restart();
                } else {
                    err = dongle_wifi_display_start();
                    if (err == ESP_OK) {
                        pressed = false;
                        ready_for_press = false;
                        ESP_LOGI(TAG,
                                 "Release GPIO%d before holding again to restart and exit Wi-Fi display mode",
                                 DONGLE_WIFI_DISPLAY_GPIO);
                    }
                }
                press_start_tick = now_tick;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(DONGLE_WIFI_DISPLAY_POLL_MS));
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

    ESP_LOGI(TAG, "dongle waiting for ESP-NOW tracker/blade packets");

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
            if (rx_packet.packet.type == M2P_ESPNOW_PACKET_TRACKER_IMU) {
                if (!s_dongle_wifi_display_active ||
                    dongle_wifi_display_should_store_tracker_packet(&rx_packet, esp_timer_get_time())) {
                    dongle_store_latest_packet(&rx_packet);
                }
            } else if (rx_packet.packet.type == M2P_ESPNOW_PACKET_BLADE_STATE) {
                dongle_store_blade_packet(&rx_packet);
            }
#endif

            if (s_dongle_wifi_display_active) {
                vTaskDelay(pdMS_TO_TICKS(DONGLE_WIFI_DISPLAY_RX_DRAIN_DELAY_MS));
            }
        }

#if DONGLE_ENABLE_SERIAL_OUTPUT
        now_tick = xTaskGetTickCount();
        if ((now_tick - last_print_tick) >= print_period_ticks) {
#if DONGLE_ENABLE_USB_KEYBOARD || DONGLE_ENABLE_USB_MOUSE
            if (s_dongle_hid_mutex != NULL) {
                (void)xSemaphoreTake(s_dongle_hid_mutex, portMAX_DELAY);
            }
#endif
            if (!s_dongle_wifi_display_active) {
                dongle_print_latest_states();
#if DONGLE_ENABLE_RF_INFERENCE
                dongle_run_rf_inference(esp_timer_get_time());
#endif
#if DONGLE_ENABLE_USB_TELEMETRY
                dongle_telemetry_maybe_send(esp_timer_get_time());
#endif
            }
#if DONGLE_ENABLE_USB_KEYBOARD || DONGLE_ENABLE_USB_MOUSE
            if (s_dongle_hid_mutex != NULL) {
                (void)xSemaphoreGive(s_dongle_hid_mutex);
            }
#endif
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

#if DONGLE_ENABLE_USB_MOUSE && DONGLE_ENABLE_USB_MOUSE_TEST
static void usb_mouse_test_task(void *arg)
{
    (void)arg;

    const TickType_t start_tick = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(USB_KEYBOARD_TEST_READY_TIMEOUT_MS);

    while (!usb_mouse_is_ready()) {
        if ((xTaskGetTickCount() - start_tick) >= timeout_ticks) {
            ESP_LOGW(TAG, "USB mouse test skipped: device not ready");
            vTaskDelete(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "USB mouse ready, test movement will start in %d ms",
             USB_KEYBOARD_TEST_START_DELAY_MS + USB_MOUSE_TEST_START_OFFSET_MS);
    vTaskDelay(pdMS_TO_TICKS(USB_KEYBOARD_TEST_START_DELAY_MS + USB_MOUSE_TEST_START_OFFSET_MS));

    esp_err_t err = ESP_OK;

    err = usb_mouse_move(-USB_MOUSE_TEST_LEFT_DELTA, 0, 0, 0);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(USB_MOUSE_TEST_STEP_MS));
        err = usb_mouse_move(USB_MOUSE_TEST_RIGHT_DELTA, 0, 0, 0);
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(USB_MOUSE_TEST_STEP_MS));
        err = usb_mouse_move(0, -USB_MOUSE_TEST_UP_DELTA, 0, 0);
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(USB_MOUSE_TEST_STEP_MS));
        err = usb_mouse_move(0, USB_MOUSE_TEST_DOWN_DELTA, 0, 0);
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(USB_MOUSE_TEST_STEP_MS));
        err = usb_mouse_click(USB_MOUSE_BUTTON_LEFT, USB_KEYBOARD_TEST_HOLD_MS);
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "USB mouse test failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "USB mouse test finished");
    }

    vTaskDelete(NULL);
}
#endif

static esp_err_t blade_button_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << (uint32_t)BLADE_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    return gpio_config(&io_conf);
}

static bool blade_button_is_pressed(void)
{
    return gpio_get_level(BLADE_BUTTON_GPIO) == 0;
}

static uint32_t elapsed_ms_since(TickType_t start_tick, TickType_t now_tick)
{
    return (uint32_t)((now_tick - start_tick) * portTICK_PERIOD_MS);
}

static esp_err_t blade_enable_deep_sleep_wakeup(void)
{
    const uint64_t wake_mask = (1ULL << (uint32_t)BLADE_BUTTON_GPIO);

    esp_err_t err = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_sleep_set_pull_mode(BLADE_BUTTON_GPIO, GPIO_PULLUP_ONLY);
    if (err != ESP_OK) {
        return err;
    }

    return esp_sleep_enable_ext1_wakeup_io(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);
}

static void blade_enter_deep_sleep(const char *reason)
{
    const char *sleep_reason = (reason != NULL) ? reason : "unknown";

    ESP_LOGI(TAG,
             "Blade entering deep sleep: reason=%s, wake=GPIO%d low",
             sleep_reason,
             BLADE_BUTTON_GPIO);
    printf("# blade-sleep: reason=%s wake_gpio=%d wake_level=low\n",
           sleep_reason,
           BLADE_BUTTON_GPIO);

    status_led_off();
    (void)max30102_shutdown();
    (void)blade_button_init();

    esp_err_t wake_err = blade_enable_deep_sleep_wakeup();
    if (wake_err != ESP_OK) {
        ESP_LOGE(TAG, "Blade deep sleep wakeup config failed: %s", esp_err_to_name(wake_err));
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}

typedef struct {
    float dc;
    float envelope;
    float bpm;
    int64_t last_beat_us;
    int64_t last_sample_us;
    uint32_t sample_count;
    uint32_t beat_count;
    bool finger_present;
    bool peak_armed;
} blade_heart_rate_state_t;

static void blade_heart_rate_reset(blade_heart_rate_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->peak_armed = true;
}

static void blade_heart_rate_process_sample(blade_heart_rate_state_t *state,
                                            const max30102_sample_t *sample,
                                            int64_t now_us)
{
    const bool finger_present = sample->ir >= BLADE_HEART_RATE_FINGER_MIN_IR;
    if (!finger_present) {
        if (state->finger_present) {
            ESP_LOGI(TAG, "heart-rate: finger removed");
        }
        blade_heart_rate_reset(state);
        state->last_sample_us = now_us;
        return;
    }

    if (!state->finger_present) {
        ESP_LOGI(TAG, "heart-rate: finger detected");
        blade_heart_rate_reset(state);
        state->finger_present = true;
        state->dc = (float)sample->ir;
    }

    state->finger_present = true;
    state->sample_count++;
    state->dc += ((float)sample->ir - state->dc) * 0.03f;
    const float ac = (float)sample->ir - state->dc;
    const float abs_ac = (ac < 0.0f) ? -ac : ac;
    state->envelope += (abs_ac - state->envelope) * 0.10f;

    /* MAX30102 samples are averaged in the sensor; this is intentionally a
     * conservative adaptive threshold to reject motion and DC drift. */
    const float threshold = (state->envelope * 0.45f > 120.0f) ?
        state->envelope * 0.45f : 120.0f;
    const bool local_maximum = state->peak_armed && ac > threshold;
    const int64_t since_last_beat =
        (state->last_beat_us > 0) ? now_us - state->last_beat_us : INT64_MAX;

    if (local_maximum && since_last_beat >= 300000) {
        /* Use a short refractory period and the adaptive envelope so the
         * dicrotic notch is not reported as a second beat. */
        if (state->last_beat_us > 0 && since_last_beat <= 1500000) {
            const float instant_bpm = 60000000.0f / (float)since_last_beat;
            if (instant_bpm >= 40.0f && instant_bpm <= 200.0f) {
                state->bpm = (state->bpm <= 0.0f) ? instant_bpm :
                            state->bpm * 0.75f + instant_bpm * 0.25f;
                state->beat_count++;
            }
        }
        state->last_beat_us = now_us;
        state->peak_armed = false;
    }
    if (ac < threshold * 0.25f) {
        state->peak_armed = true;
    }
    if (state->last_beat_us > 0 && now_us - state->last_beat_us > 3000000) {
        state->bpm = 0.0f;
    }
    state->last_sample_us = now_us;
}

static void blade_heart_rate_task(void *arg)
{
    (void)arg;
    blade_heart_rate_state_t state;
    blade_heart_rate_reset(&state);
    max30102_sample_t last_sample = {0};
    int64_t last_serial_us = 0;
    int64_t last_error_us = 0;

    printf("# heart-rate: sensor=MAX30102 address=0x57 SDA=%d SCL=%d INT=%d\n",
           BLADE_HEART_RATE_SDA_GPIO,
           BLADE_HEART_RATE_SCL_GPIO,
           BLADE_HEART_RATE_INT_GPIO);
    printf("heart_rate_ms,bpm,finger_present,ir,red\n");

    while (1) {
        esp_err_t read_err = ESP_OK;
        while (read_err == ESP_OK) {
            max30102_sample_t sample = {0};
            read_err = max30102_read_sample(&sample);
            if (read_err == ESP_OK) {
                last_sample = sample;
                blade_heart_rate_process_sample(&state, &sample, esp_timer_get_time());
            }
        }

        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_serial_us >= (int64_t)BLADE_HEART_RATE_SERIAL_MS * 1000LL) {
            printf("%" PRId64 ",%.1f,%d,%" PRIu32 ",%" PRIu32 "\n",
                   (int64_t)(now_us / 1000LL),
                   state.finger_present ? (double)state.bpm : 0.0,
                   state.finger_present ? 1 : 0,
                   last_sample.ir,
                   last_sample.red);
            last_serial_us = now_us;
        }

        if (read_err != ESP_ERR_NOT_FOUND && now_us - last_error_us >= 1000000) {
            ESP_LOGW(TAG, "MAX30102 FIFO read failed: %s", esp_err_to_name(read_err));
            last_error_us = now_us;
        }
        vTaskDelay(pdMS_TO_TICKS(BLADE_HEART_RATE_POLL_MS));
    }
}

static bool blade_woke_from_button_sleep(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    return cause == ESP_SLEEP_WAKEUP_EXT1 || cause == ESP_SLEEP_WAKEUP_GPIO;
}

static void blade_confirm_wake_hold_or_sleep(void)
{
    if (!blade_woke_from_button_sleep()) {
        return;
    }

    ESP_LOGI(TAG,
             "Blade woke by button, hold GPIO%d for %d ms to start",
             BLADE_BUTTON_GPIO,
             BLADE_WAKE_CONFIRM_HOLD_MS);
    printf("# blade-wake: hold_required_ms=%d\n", BLADE_WAKE_CONFIRM_HOLD_MS);

    TickType_t start_tick = xTaskGetTickCount();
    while (elapsed_ms_since(start_tick, xTaskGetTickCount()) < BLADE_WAKE_CONFIRM_HOLD_MS) {
        if (!blade_button_is_pressed()) {
            ESP_LOGI(TAG, "Blade wake hold released before confirmation");
            blade_enter_deep_sleep("wake_hold_released");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(BLADE_POLL_PERIOD_MS));
    }

    ESP_LOGI(TAG, "Blade wake hold confirmed, starting normal mode");
    printf("# blade-wake: confirmed=1\n");
}

typedef enum {
    BLADE_SLEEP_GESTURE_IDLE = 0,
    BLADE_SLEEP_GESTURE_COLLECT_SHORT_PRESSES,
    BLADE_SLEEP_GESTURE_FINAL_HOLD,
} blade_sleep_gesture_state_t;

static void blade_report_task(void *arg)
{
    (void)arg;

    TickType_t last_wake = xTaskGetTickCount();
    TickType_t last_report_tick = 0;
    TickType_t last_send_error_log_tick = 0;
    uint32_t sequence = 0;
    uint8_t report_burst_remaining = BLADE_STATE_CHANGE_BURST_COUNT;
    bool last_raw_pressed = blade_button_is_pressed();
    bool stable_pressed = last_raw_pressed;
    uint8_t same_sample_count = BLADE_DEBOUNCE_SAMPLES;
    blade_sleep_gesture_state_t sleep_gesture_state = BLADE_SLEEP_GESTURE_IDLE;
    uint8_t sleep_short_press_count = 0;
    TickType_t press_start_tick = last_wake;
    TickType_t last_short_release_tick = 0;
    TickType_t final_hold_start_tick = 0;
#if BLADE_ENABLE_SERIAL_OUTPUT
    TickType_t last_serial_tick = 0;
#endif

    led_set(stable_pressed);
    ESP_LOGI(TAG, "blade button initial state: %s",
             stable_pressed ? "pressed" : "released");

    while (1) {
        bool state_changed = false;
        const int gpio_level = gpio_get_level(BLADE_BUTTON_GPIO);
        const bool raw_pressed = gpio_level == 0;
        const TickType_t now_tick = xTaskGetTickCount();
        if (raw_pressed == last_raw_pressed) {
            if (same_sample_count < BLADE_DEBOUNCE_SAMPLES) {
                same_sample_count++;
            }
        } else {
            last_raw_pressed = raw_pressed;
            same_sample_count = 1;
        }

        if (same_sample_count >= BLADE_DEBOUNCE_SAMPLES &&
            stable_pressed != last_raw_pressed) {
            stable_pressed = last_raw_pressed;
            state_changed = true;
            led_set(stable_pressed);
            ESP_LOGI(TAG, "blade button state: %s",
                     stable_pressed ? "pressed" : "released");
        }

        if (state_changed) {
            if (stable_pressed) {
                press_start_tick = now_tick;
                if (sleep_gesture_state == BLADE_SLEEP_GESTURE_COLLECT_SHORT_PRESSES &&
                    sleep_short_press_count >= BLADE_SLEEP_SHORT_PRESS_COUNT &&
                    elapsed_ms_since(last_short_release_tick, now_tick) <= BLADE_SLEEP_SEQUENCE_GAP_MS) {
                    sleep_gesture_state = BLADE_SLEEP_GESTURE_FINAL_HOLD;
                    final_hold_start_tick = now_tick;
                    ESP_LOGI(TAG,
                             "Blade sleep gesture: final hold detected after %u short presses, hold %d ms to sleep",
                             sleep_short_press_count,
                             BLADE_SLEEP_HOLD_MS);
                    printf("# blade-sleep-gesture: stage=final_hold short_count=%u hold_required_ms=%d\n",
                           sleep_short_press_count,
                           BLADE_SLEEP_HOLD_MS);
                }
            } else {
                const uint32_t press_ms = elapsed_ms_since(press_start_tick, now_tick);
                if (sleep_gesture_state == BLADE_SLEEP_GESTURE_FINAL_HOLD) {
                    sleep_gesture_state = BLADE_SLEEP_GESTURE_IDLE;
                    sleep_short_press_count = 0;
                    ESP_LOGI(TAG, "Blade sleep gesture cancelled before long hold");
                    printf("# blade-sleep-gesture: stage=cancelled press_ms=%" PRIu32 "\n",
                           press_ms);
                } else if (press_ms <= BLADE_SLEEP_SHORT_PRESS_MAX_MS) {
                    if (sleep_gesture_state == BLADE_SLEEP_GESTURE_COLLECT_SHORT_PRESSES &&
                        elapsed_ms_since(last_short_release_tick, now_tick) <= BLADE_SLEEP_SEQUENCE_GAP_MS) {
                        if (sleep_short_press_count < UINT8_MAX) {
                            sleep_short_press_count++;
                        }
                    } else {
                        sleep_short_press_count = 1;
                    }

                    sleep_gesture_state = BLADE_SLEEP_GESTURE_COLLECT_SHORT_PRESSES;
                    last_short_release_tick = now_tick;
                    ESP_LOGI(TAG,
                             "Blade sleep gesture: short press %u/%u, next press must start within %d ms",
                             sleep_short_press_count,
                             BLADE_SLEEP_SHORT_PRESS_COUNT,
                             BLADE_SLEEP_SEQUENCE_GAP_MS);
                    printf("# blade-sleep-gesture: stage=short_press count=%u required=%u press_ms=%" PRIu32 "\n",
                           sleep_short_press_count,
                           BLADE_SLEEP_SHORT_PRESS_COUNT,
                           press_ms);
                } else if (sleep_gesture_state != BLADE_SLEEP_GESTURE_IDLE) {
                    sleep_gesture_state = BLADE_SLEEP_GESTURE_IDLE;
                    sleep_short_press_count = 0;
                    ESP_LOGI(TAG, "Blade sleep gesture cancelled by long non-final press");
                    printf("# blade-sleep-gesture: stage=cancelled_long_press press_ms=%" PRIu32 "\n",
                           press_ms);
                }
            }
        }

        if (sleep_gesture_state == BLADE_SLEEP_GESTURE_COLLECT_SHORT_PRESSES &&
            elapsed_ms_since(last_short_release_tick, now_tick) > BLADE_SLEEP_SEQUENCE_GAP_MS) {
            sleep_gesture_state = BLADE_SLEEP_GESTURE_IDLE;
            sleep_short_press_count = 0;
        }

        if (sleep_gesture_state == BLADE_SLEEP_GESTURE_FINAL_HOLD &&
            stable_pressed &&
            elapsed_ms_since(final_hold_start_tick, now_tick) >= BLADE_SLEEP_HOLD_MS) {
            blade_enter_deep_sleep("quad_click_long_hold");
        }

        if (state_changed) {
            report_burst_remaining = BLADE_STATE_CHANGE_BURST_COUNT;
        }

        const uint32_t report_period_ms = stable_pressed ?
            BLADE_PRESSED_REPORT_PERIOD_MS : BLADE_IDLE_HEARTBEAT_PERIOD_MS;
        const bool periodic_report_due =
            elapsed_ms_since(last_report_tick, now_tick) >= report_period_ms;
        const bool should_report = (report_burst_remaining > 0) || periodic_report_due;

        if (should_report) {
            esp_err_t send_err = ESP_OK;
#if MOVE_TO_PLAY_ENABLE_ESPNOW
            send_err = m2p_espnow_send_blade_state(BLADE_NODE_ID,
                                                   sequence,
                                                   stable_pressed,
                                                   s_local_battery_valid,
                                                   s_local_battery_percent,
                                                   s_local_battery_mv);
            if (send_err != ESP_OK &&
                (last_send_error_log_tick == 0 ||
                 elapsed_ms_since(last_send_error_log_tick, now_tick) >= 1000U)) {
                ESP_LOGW(TAG,
                         "ESP-NOW blade state send failed: %s",
                         esp_err_to_name(send_err));
                last_send_error_log_tick = now_tick;
            }
#endif
#if BLADE_ENABLE_SERIAL_OUTPUT
            if (state_changed ||
                (now_tick - last_serial_tick) >= pdMS_TO_TICKS(BLADE_SERIAL_STATE_PERIOD_MS)) {
                printf("# blade-tx: node_id=%u pressed=%d raw_pressed=%d gpio=%d"
                       " seq=%" PRIu32 " battery_valid=%d battery=%u%% battery_mv=%u send=%s\n",
                       BLADE_NODE_ID,
                       stable_pressed ? 1 : 0,
                       raw_pressed ? 1 : 0,
                       gpio_level,
                       sequence,
                       s_local_battery_valid ? 1 : 0,
                       s_local_battery_valid ? s_local_battery_percent : 0,
                       s_local_battery_valid ? s_local_battery_mv : 0,
                       esp_err_to_name(send_err));
                last_serial_tick = now_tick;
            }
#endif
            if (report_burst_remaining > 0) {
                report_burst_remaining--;
            }
            last_report_tick = now_tick;
            sequence++;
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(BLADE_POLL_PERIOD_MS));
    }
}

static void start_dongle_mode(void)
{
    ESP_LOGI(TAG, "Starting dongle mode");
    ESP_LOGI(TAG, "role: receive tracker data");
    ESP_LOGI(TAG, "esp-now channel=%d", M2P_ESPNOW_CHANNEL);
    ESP_LOGI(TAG, "serial output=%d", DONGLE_ENABLE_SERIAL_OUTPUT);
#if DONGLE_ENABLE_SERIAL_OUTPUT
    ESP_LOGI(TAG, "serial latest-state rate=%d Hz", DONGLE_SERIAL_STATE_RATE_HZ);
    ESP_LOGI(TAG, "raw csv output=%d", DONGLE_ENABLE_RAW_CSV_OUTPUT);
    ESP_LOGI(TAG, "serial age column=%d", DONGLE_ENABLE_SERIAL_AGE_COLUMN);
    ESP_LOGI(TAG, "rf inference=%d", DONGLE_ENABLE_RF_INFERENCE);
#endif
    ESP_LOGI(TAG, "usb keyboard=%d", DONGLE_ENABLE_USB_KEYBOARD);
    ESP_LOGI(TAG, "usb mouse=%d", DONGLE_ENABLE_USB_MOUSE);
    ESP_LOGI(TAG, "usb cdc telemetry=%d", DONGLE_ENABLE_USB_TELEMETRY);

    led_set(true);

#if DONGLE_ENABLE_SERIAL_OUTPUT
    if (s_dongle_state_mutex == NULL) {
        s_dongle_state_mutex = xSemaphoreCreateMutex();
        if (s_dongle_state_mutex == NULL) {
            ESP_LOGW(TAG, "dongle state mutex alloc failed");
            return;
        }
    }
#endif

#if DONGLE_ENABLE_USB_KEYBOARD || DONGLE_ENABLE_USB_MOUSE
    if (s_dongle_hid_mutex == NULL) {
        s_dongle_hid_mutex = xSemaphoreCreateMutex();
        if (s_dongle_hid_mutex == NULL) {
            ESP_LOGW(TAG, "dongle HID mutex alloc failed");
            return;
        }
    }
#endif

#if MOVE_TO_PLAY_ENABLE_ESPNOW
    esp_err_t espnow_err = m2p_espnow_init(M2P_ESPNOW_ROLE_DONGLE, M2P_ESPNOW_CHANNEL);
    if (espnow_err != ESP_OK) {
        ESP_LOGW(TAG, "ESP-NOW init failed: %s", esp_err_to_name(espnow_err));
        return;
    }

#if DONGLE_ENABLE_RF_INFERENCE && DONGLE_ENABLE_HID_ACTION_LOG
    (void)dongle_action_config_load();
#endif

    xTaskCreatePinnedToCore(espnow_rx_task,
                            "espnow_rx_task",
                            4096,
                            NULL,
                            7,
                            NULL,
                            tskNO_AFFINITY);

#if DONGLE_ENABLE_SERIAL_OUTPUT
    xTaskCreatePinnedToCore(dongle_wifi_display_button_task,
                            "dongle_wifi_btn",
                            3072,
                            NULL,
                            4,
                            NULL,
                            tskNO_AFFINITY);
#endif
#else
    ESP_LOGI(TAG, "ESP-NOW disabled by MOVE_TO_PLAY_ENABLE_ESPNOW");
#endif

#if DONGLE_ENABLE_USB_KEYBOARD || DONGLE_ENABLE_USB_MOUSE
    esp_err_t usb_err = usb_keyboard_init();
    if (usb_err != ESP_OK) {
        ESP_LOGW(TAG, "USB HID init failed: %s", esp_err_to_name(usb_err));
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

#if DONGLE_ENABLE_USB_MOUSE_TEST
    xTaskCreatePinnedToCore(usb_mouse_test_task,
                            "usb_mouse_test",
                            3072,
                            NULL,
                            5,
                            NULL,
                            tskNO_AFFINITY);
#endif
#else
    ESP_LOGI(TAG, "USB HID disabled for ESP-NOW serial test");
#endif
}

static void start_tracker_mode(void)
{
    ESP_LOGI(TAG, "Starting tracker mode");
    ESP_LOGI(TAG, "role: read IMU and send tracker data");
    ESP_LOGI(TAG, "tracker board style=%d (%s)",
             MOVE_TO_PLAY_TRACKER_BOARD_STYLE,
             MOVE_TO_PLAY_TRACKER_BOARD_NAME);
    ESP_LOGI(TAG, "node_id=%d", BOARD_NODE_ID);
    ESP_LOGI(TAG, "node_name=%s", tracker_node_name(BOARD_NODE_ID));
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

    xTaskCreatePinnedToCore(imu_sampling_task,
                            "imu_sampling_task",
                            4096,
                            NULL,
                            8,
                            NULL,
                            tskNO_AFFINITY);
}

static void start_blade_mode(void)
{
    ESP_LOGI(TAG, "Starting Blade mode");
    ESP_LOGI(TAG, "role: read GPIO%d active-low button and send Blade state",
             BLADE_BUTTON_GPIO);
    ESP_LOGI(TAG, "node_id=%d", BLADE_NODE_ID);
    ESP_LOGI(TAG, "esp-now channel=%d", M2P_ESPNOW_CHANNEL);
    ESP_LOGI(TAG,
             "poll_rate_hz=%d idle_heartbeat_hz=%d pressed_report_hz=%d burst_count=%d",
             BLADE_POLL_RATE_HZ,
             BLADE_IDLE_HEARTBEAT_RATE_HZ,
             BLADE_PRESSED_REPORT_RATE_HZ,
             BLADE_STATE_CHANGE_BURST_COUNT);

    led_set(false);

    esp_err_t button_err = blade_button_init();
    if (button_err != ESP_OK) {
        ESP_LOGE(TAG, "Blade button GPIO init failed: %s", esp_err_to_name(button_err));
        return;
    }

    blade_confirm_wake_hold_or_sleep();

#if MOVE_TO_PLAY_ENABLE_ESPNOW
    esp_err_t espnow_err = m2p_espnow_init(M2P_ESPNOW_ROLE_BLADE, M2P_ESPNOW_CHANNEL);
    if (espnow_err != ESP_OK) {
        ESP_LOGW(TAG, "ESP-NOW init failed: %s", esp_err_to_name(espnow_err));
    }
#else
    ESP_LOGI(TAG, "ESP-NOW disabled by MOVE_TO_PLAY_ENABLE_ESPNOW");
#endif

    esp_err_t heart_rate_err = max30102_init(BLADE_HEART_RATE_SDA_GPIO,
                                             BLADE_HEART_RATE_SCL_GPIO,
                                             BLADE_HEART_RATE_INT_GPIO);
    if (heart_rate_err == ESP_OK) {
        xTaskCreatePinnedToCore(blade_heart_rate_task,
                                "blade_heart_rate",
                                4096,
                                NULL,
                                5,
                                NULL,
                                tskNO_AFFINITY);
    } else {
        ESP_LOGW(TAG,
                 "Blade heart-rate sensor disabled: %s (check MAX30102 power and I2C wiring)",
                 esp_err_to_name(heart_rate_err));
    }

    xTaskCreatePinnedToCore(blade_report_task,
                            "blade_report_task",
                            3072,
                            NULL,
                            6,
                            NULL,
                            tskNO_AFFINITY);
}

static void battery_monitor_task(void *arg)
{
    (void)arg;

    const uint8_t battery_node_id =
        (MOVE_TO_PLAY_DEVICE_MODE == MOVE_TO_PLAY_MODE_BLADE) ? BLADE_NODE_ID : BOARD_NODE_ID;
    const char *battery_node_name =
        (MOVE_TO_PLAY_DEVICE_MODE == MOVE_TO_PLAY_MODE_BLADE) ? "blade" : tracker_node_name(BOARD_NODE_ID);

    battery_monitor_init(BATTERY_ADC_UNIT, BATTERY_ADC_CHANNEL, BATTERY_ADC_GPIO);
    vTaskDelay(pdMS_TO_TICKS(500));

    while (1) {
        float voltage = battery_monitor_get_voltage();
        int percent = battery_monitor_percent_from_voltage(voltage);
        uint16_t voltage_mv = battery_voltage_to_mv(voltage);

        if (voltage > 0.0f) {
            s_local_battery_percent = (uint8_t)percent;
            s_local_battery_mv = voltage_mv;
            s_local_battery_valid = true;
        } else {
            s_local_battery_valid = false;
        }

        printf("# battery: node_id=%d node_name=%s board_style=%d board_name=%s %.2fV  %d%%\n",
               battery_node_id,
               battery_node_name,
               MOVE_TO_PLAY_TRACKER_BOARD_STYLE,
               MOVE_TO_PLAY_TRACKER_BOARD_NAME,
               (double)voltage,
               percent);
        if (MOVE_TO_PLAY_DEVICE_MODE == MOVE_TO_PLAY_MODE_TRACKER) {
            status_led_set_battery_color(percent);
        }

        vTaskDelay(pdMS_TO_TICKS(BATTERY_REPORT_INTERVAL_MS));
    }
}

void app_main(void)
{
    led_init();

    const esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    const bool blade_button_wakeup =
        MOVE_TO_PLAY_DEVICE_MODE == MOVE_TO_PLAY_MODE_BLADE &&
        (wake_cause == ESP_SLEEP_WAKEUP_EXT1 || wake_cause == ESP_SLEEP_WAKEUP_GPIO);
    if (!blade_button_wakeup) {
        led_blink_startup(3, 120, 120);
    }

    ESP_LOGI(TAG, "Booting MoveToPlay");
    ESP_LOGI(TAG, "device_mode=%d (0=dongle, 1=tracker, 2=blade)",
             MOVE_TO_PLAY_DEVICE_MODE);

    if (MOVE_TO_PLAY_DEVICE_MODE == MOVE_TO_PLAY_MODE_DONGLE) {
        start_dongle_mode();
    } else if (MOVE_TO_PLAY_DEVICE_MODE == MOVE_TO_PLAY_MODE_TRACKER) {
        xTaskCreatePinnedToCore(battery_monitor_task,
                                "battery_task",
                                3072,
                                NULL,
                                3,
                                NULL,
                                tskNO_AFFINITY);
        start_tracker_mode();
    } else {
        xTaskCreatePinnedToCore(battery_monitor_task,
                                "battery_task",
                                3072,
                                NULL,
                                3,
                                NULL,
                                tskNO_AFFINITY);
        start_blade_mode();
    }
}
