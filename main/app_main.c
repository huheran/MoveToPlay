#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "imu_lsm6dsv.h"
#include "m2p_espnow.h"
#include "rf_infer.h"
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
#define M2P_BOARD_PROFILE             1
#define M2P_DONGLE_MODE               2

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
#define DONGLE_MAX_TRACKER_NODES      8
#define DONGLE_RF_MAX_NODE_AGE_MS     250
#define DONGLE_RF_PRINT_INTERVAL_MS   120
#define DONGLE_RF_MIN_CONFIDENCE      0.60f
#define DONGLE_BLADE_MAX_AGE_MS       300
#define DONGLE_BLADE_TURN_PERIOD_MS   20
#define DONGLE_BLADE_TURN_CHEST_NODE_ID TRACKER_NODE_CHEST
/* 0=gx, 1=gy, 2=gz. Current turn data shows chest gy has the strongest left/right yaw signal. */
#define DONGLE_BLADE_TURN_GYRO_AXIS   1
/* Mouse X is positive for turning right. With the current chest mounting, gy positive means left. */
#define DONGLE_BLADE_TURN_GYRO_SIGN   -1.0f
#define DONGLE_BLADE_TURN_DEADZONE_DPS 8.0f
#define DONGLE_BLADE_TURN_SENSITIVITY 6.0f /* mouse px per integrated gyro degree */
#define DONGLE_BLADE_TURN_MAX_STEP_DELTA 80
#define DONGLE_BLADE_TURN_CHEST_MAX_AGE_MS 150

#define BLADE_NODE_ID                 100
#define BLADE_BUTTON_GPIO             GPIO_NUM_4
#define BLADE_REPORT_RATE_HZ          50
#define BLADE_REPORT_PERIOD_MS        (1000 / BLADE_REPORT_RATE_HZ)
#define BLADE_DEBOUNCE_SAMPLES        3
#define BLADE_ENABLE_SERIAL_OUTPUT    1
#define BLADE_SERIAL_STATE_RATE_HZ    10
#define BLADE_SERIAL_STATE_PERIOD_MS  (1000 / BLADE_SERIAL_STATE_RATE_HZ)
#define BLADE_SLEEP_FIRST_CLICK_MAX_MS 600
#define BLADE_SLEEP_DOUBLE_CLICK_GAP_MS 700
#define BLADE_SLEEP_HOLD_MS           5000
#define BLADE_WAKE_CONFIRM_HOLD_MS    3000

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

#if (MOVE_TO_PLAY_TRACKER_NODE_ID < 1) || (MOVE_TO_PLAY_TRACKER_NODE_ID > DONGLE_MAX_TRACKER_NODES)
#error "MOVE_TO_PLAY_TRACKER_NODE_ID must be in range 1..DONGLE_MAX_TRACKER_NODES"
#endif

/* 预留后续按键/串口命令控制，第一版默认开启采样 */
static bool sampling_enabled = true;

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
            esp_err_t send_err = m2p_espnow_send_tracker_sample(BOARD_NODE_ID, sample_index, &sample);
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
    uint8_t src_addr[6];
    int64_t last_rx_us;
    m2p_espnow_tracker_packet_t packet;
} dongle_latest_node_t;

typedef struct {
    bool valid;
    bool pressed;
    bool dirty;
    uint8_t src_addr[6];
    uint8_t node_id;
    uint32_t sequence;
    uint32_t timestamp_us;
    int64_t last_rx_us;
} dongle_blade_state_t;

static dongle_latest_node_t s_dongle_latest_nodes[DONGLE_MAX_TRACKER_NODES];
static dongle_blade_state_t s_dongle_blade_state;
#if DONGLE_ENABLE_USB_MOUSE
static int64_t s_dongle_last_blade_turn_us = 0;
static float s_dongle_blade_turn_dx_remainder = 0.0f;
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
}

static void dongle_store_blade_packet(const m2p_espnow_rx_packet_t *rx_packet)
{
    if (rx_packet->packet.type != M2P_ESPNOW_PACKET_BLADE_STATE) {
        return;
    }

    const bool pressed =
        (rx_packet->packet.flags & M2P_ESPNOW_BLADE_FLAG_PRESSED) != 0;
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
    memcpy(s_dongle_blade_state.src_addr,
           rx_packet->src_addr,
           sizeof(s_dongle_blade_state.src_addr));
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
static float dongle_blade_turn_gyro_dps(const m2p_espnow_tracker_packet_t *packet)
{
    switch (DONGLE_BLADE_TURN_GYRO_AXIS) {
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

    float gyro_dps = dongle_blade_turn_gyro_dps(chest_packet) * DONGLE_BLADE_TURN_GYRO_SIGN;
    if (gyro_dps > -DONGLE_BLADE_TURN_DEADZONE_DPS &&
        gyro_dps < DONGLE_BLADE_TURN_DEADZONE_DPS) {
        gyro_dps = 0.0f;
    }

    const float dt_s = (float)elapsed_us / 1000000.0f;
    float dx_f = (gyro_dps * dt_s * DONGLE_BLADE_TURN_SENSITIVITY) +
                 s_dongle_blade_turn_dx_remainder;
    int16_t dx = (int16_t)((dx_f >= 0.0f) ? (dx_f + 0.5f) : (dx_f - 0.5f));
    bool dx_saturated = false;
    if (dx > DONGLE_BLADE_TURN_MAX_STEP_DELTA) {
        dx = DONGLE_BLADE_TURN_MAX_STEP_DELTA;
        dx_saturated = true;
    } else if (dx < -DONGLE_BLADE_TURN_MAX_STEP_DELTA) {
        dx = -DONGLE_BLADE_TURN_MAX_STEP_DELTA;
        dx_saturated = true;
    }
    s_dongle_blade_turn_dx_remainder = dx_saturated ? 0.0f : (dx_f - (float)dx);

    if (dx == 0) {
        s_dongle_last_blade_turn_us = now_us;
        return;
    }

    esp_err_t err = ESP_OK;
    if (!usb_mouse_is_ready()) {
        return;
    }
    err = usb_mouse_move((int8_t)dx, 0, 0, 0);
    if (err == ESP_OK) {
        s_dongle_last_blade_turn_us = now_us;
    }

#if DONGLE_ENABLE_HID_EVENT_LOG
    printf("# blade-hid: pressed=1 chest_node=%u axis=%d gyro_dps=%.1f"
           " chest_age_ms=%.1f dx=%d result=%s\n",
           (unsigned)DONGLE_BLADE_TURN_CHEST_NODE_ID,
           DONGLE_BLADE_TURN_GYRO_AXIS,
           (double)gyro_dps,
           chest_age_ms,
           dx,
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
#define DONGLE_BLADE_MOVE_HOLD_GRACE_MS 300
#define DONGLE_CONFIRM_FRAMES         3
#define DONGLE_INFER_RATE_HZ          25

#define DONGLE_ARRAY_SIZE(a)          (sizeof(a) / sizeof((a)[0]))

#define HID_KEY_1       0x1E
#define HID_KEY_2       0x1F
#define HID_KEY_3       0x20
#define HID_KEY_4       0x21
#define HID_KEY_E       0x08
#define HID_KEY_F       0x09
#define HID_KEY_Q       0x14
#define HID_KEY_W       0x1A
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
} dongle_action_type_t;

typedef enum {
    TRIGGER_COOLDOWN,
    TRIGGER_EDGE,
    TRIGGER_SUSTAIN,
    TRIGGER_REPEAT,
} dongle_trigger_mode_t;

typedef struct {
    dongle_action_type_t type;
    uint8_t modifier;
    uint8_t keycode;
    dongle_trigger_mode_t trigger;
    uint16_t cooldown_ms;
    uint16_t sustain_frames;
} dongle_key_action_t;

/* RF class order from sklearn string labels:
   hands_cross_forehead(0), hands_press_down(1), hands_shoot(2), idle(3),
   jump(4), kick(5), left_hand_raise(6), move_noise(7),
   right_hand_raise(8), right_hand_slash(9), run(10), turn_left(11),
   turn_right(12), ultraman_beam(13), walk(14) */
static const dongle_key_action_t s_class_key_actions[RF_MODEL_CLASS_COUNT] = {
    [0] = { ACTION_TYPE_CHARACTER_CYCLE, 0, 0, TRIGGER_EDGE, 800, 0 },       /* hands_cross_forehead -> 1/2/3/4 */
    [1] = { ACTION_TYPE_KEY_TAP, 0, HID_KEY_F, TRIGGER_COOLDOWN, 600, 0 },   /* hands_press_down -> F */
    [2] = { ACTION_TYPE_MOUSE_RIGHT_TIMED_HOLD, 0, 0, TRIGGER_EDGE, 500, 0 }, /* hands_shoot -> hold right mouse 500ms */
    [3] = { ACTION_TYPE_NONE, 0, 0, TRIGGER_COOLDOWN, 0, 0 },                /* idle */
    [4] = { ACTION_TYPE_KEY_TAP, 0, HID_KEY_SPACE, TRIGGER_EDGE, 1000, 0 },  /* jump -> SPACE */
    [5] = { ACTION_TYPE_KEY_TAP, 0, HID_KEY_E, TRIGGER_COOLDOWN, 1000, 0 },  /* kick -> E */
    [6] = { ACTION_TYPE_NONE, 0, 0, TRIGGER_COOLDOWN, 0, 0 },                /* left_hand_raise: view turn is handled by Blade + chest gyro */
    [7] = { ACTION_TYPE_NONE, 0, 0, TRIGGER_COOLDOWN, 0, 0 },                /* move_noise */
    [8] = { ACTION_TYPE_NONE, 0, 0, TRIGGER_COOLDOWN, 0, 0 },                /* right_hand_raise: view turn is handled by Blade + chest gyro */
    [9] = { ACTION_TYPE_MOUSE_CLICK, 0, 0, TRIGGER_REPEAT, DONGLE_SLASH_CLICK_INTERVAL_MS, 1 }, /* right_hand_slash -> left click every 500ms */
    [10] = { ACTION_TYPE_KEY_HOLD, USB_KEYBOARD_MOD_LEFT_SHIFT, HID_KEY_W, TRIGGER_COOLDOWN, 0, 0 }, /* run -> Shift+W */
    [11] = { ACTION_TYPE_MOUSE_TURN_LEFT, 0, 0, TRIGGER_REPEAT, DONGLE_VIEW_ACTION_INTERVAL_MS, 1 }, /* turn_left -> mouse left */
    [12] = { ACTION_TYPE_MOUSE_TURN_RIGHT, 0, 0, TRIGGER_REPEAT, DONGLE_VIEW_ACTION_INTERVAL_MS, 1 }, /* turn_right -> mouse right */
    [13] = { ACTION_TYPE_KEY_TAP, 0, HID_KEY_Q, TRIGGER_COOLDOWN, 1000, 0 }, /* ultraman_beam -> Q */
    [14] = { ACTION_TYPE_KEY_HOLD, 0, HID_KEY_W, TRIGGER_COOLDOWN, 0, 0 },   /* walk -> W */
};
#define DONGLE_NUM_CLASSES RF_MODEL_CLASS_COUNT
#define DONGLE_IDLE_CLASS 3
#define DONGLE_LEFT_HAND_RAISE_CLASS 6
#define DONGLE_MOVE_NOISE_CLASS 7
#define DONGLE_RIGHT_HAND_RAISE_CLASS 8
#define DONGLE_RIGHT_HAND_SLASH_CLASS 9
#define DONGLE_RUN_CLASS 10
#define DONGLE_TURN_LEFT_CLASS 11
#define DONGLE_TURN_RIGHT_CLASS 12
#define DONGLE_WALK_CLASS 14

#if DONGLE_ENABLE_USB_KEYBOARD
#define DONGLE_HID_LOG_PREFIX "# hid"
#else
#define DONGLE_HID_LOG_PREFIX "# hid-dry-run"
#endif

#if DONGLE_ENABLE_ACTION_DEBUG_OUTPUT || DONGLE_ENABLE_HID_EVENT_LOG
static const char *dongle_action_class_name(uint8_t class_idx)
{
    return (class_idx < RF_MODEL_CLASS_COUNT) ? rf_model_class_names[class_idx] : "unknown";
}
#endif

#if DONGLE_ENABLE_HID_EVENT_LOG
static const char *dongle_key_name(uint8_t keycode)
{
    switch (keycode) {
    case HID_KEY_1:
        return "1";
    case HID_KEY_2:
        return "2";
    case HID_KEY_3:
        return "3";
    case HID_KEY_4:
        return "4";
    case HID_KEY_E:
        return "E";
    case HID_KEY_F:
        return "F";
    case HID_KEY_Q:
        return "Q";
    case HID_KEY_W:
        return "W";
    case HID_KEY_SPACE:
        return "SPACE";
    default:
        return "NONE";
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
#if DONGLE_ENABLE_HID_EVENT_LOG
#if DONGLE_ENABLE_USB_KEYBOARD
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
#if DONGLE_ENABLE_HID_EVENT_LOG
#if DONGLE_ENABLE_USB_KEYBOARD
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
#if DONGLE_ENABLE_USB_KEYBOARD
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
#if DONGLE_ENABLE_USB_KEYBOARD
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
#if DONGLE_ENABLE_USB_KEYBOARD
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

static bool dongle_release_stale_keyboard_hold_during_blade_turn(int64_t now_us)
{
    if (s_dongle_keyboard_hold_class < 0) {
        return false;
    }

    const uint8_t hold_class = (uint8_t)s_dongle_keyboard_hold_class;
    if (hold_class != DONGLE_RUN_CLASS && hold_class != DONGLE_WALK_CLASS) {
        return false;
    }

    const int64_t elapsed_us = now_us - s_dongle_last_state_us;
    if (s_dongle_last_state_class != s_dongle_keyboard_hold_class ||
        elapsed_us < 0 ||
        elapsed_us > ((int64_t)DONGLE_BLADE_MOVE_HOLD_GRACE_MS * 1000LL)) {
        dongle_release_keyboard_hold();
        return true;
    }

    return false;
}

static bool dongle_is_event_class(uint8_t class_idx)
{
    return class_idx < DONGLE_NUM_CLASSES && !dongle_is_state_class(class_idx);
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

    if (s_dongle_pending_count >= DONGLE_CONFIRM_FRAMES) {
        s_dongle_confirmed_class = s_dongle_pending_class;
    }

    return (s_dongle_confirmed_class >= 0) ? (uint8_t)s_dongle_confirmed_class : DONGLE_IDLE_CLASS;
}

static void dongle_fire_action(uint8_t class_idx, const dongle_key_action_t *action, int64_t now_us)
{
    if (action->type == ACTION_TYPE_KEY_TAP) {
        esp_err_t err = ESP_OK;
#if DONGLE_ENABLE_USB_KEYBOARD
        err = usb_keyboard_tap_key(action->modifier, action->keycode, DONGLE_KEY_TAP_HOLD_MS);
#endif
        dongle_log_key_event(class_idx, "key_tap", action->modifier, action->keycode, err);
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
#if DONGLE_ENABLE_USB_KEYBOARD
        err = usb_mouse_click(USB_MOUSE_BUTTON_LEFT, DONGLE_KEY_TAP_HOLD_MS);
#endif
        dongle_log_mouse_button_event(class_idx, "mouse_click", USB_MOUSE_BUTTON_LEFT, err);
    } else if (action->type == ACTION_TYPE_MOUSE_RIGHT_TIMED_HOLD) {
        dongle_release_mouse_hold();
        dongle_release_timed_mouse_hold(true, now_us);
        esp_err_t err = ESP_OK;
#if DONGLE_ENABLE_USB_KEYBOARD
        err = usb_mouse_set_buttons(USB_MOUSE_BUTTON_RIGHT);
#endif
        dongle_log_mouse_button_event(class_idx, "mouse_hold", USB_MOUSE_BUTTON_RIGHT, err);
        if (err == ESP_OK) {
            s_dongle_mouse_timed_hold_class = (int8_t)class_idx;
            s_dongle_mouse_timed_release_us =
                now_us + ((int64_t)DONGLE_MOUSE_RIGHT_HOLD_MS * 1000LL);
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

    uint8_t raw_class = infer_class;
    const bool raw_class_is_confident =
        raw_class < DONGLE_NUM_CLASSES && infer_confidence >= DONGLE_RF_MIN_CONFIDENCE;
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

    const dongle_key_action_t *action = &s_class_key_actions[class_idx];

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
            const dongle_key_action_t *a = &s_class_key_actions[i];
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
            s_dongle_hid_status = "hold_active";
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
#if DONGLE_ENABLE_USB_KEYBOARD
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
            const bool released_move = dongle_release_stale_keyboard_hold_during_blade_turn(now_us);
            s_dongle_hid_status = released_move ?
                                  "blade_turn_move_hold_expired" :
                                  "blade_turn_override";
            return;
        }
        dongle_release_mouse_hold();
        dongle_release_timed_mouse_hold(true, now_us);
    } else {
        /* Release held actions before tap/move actions */
        dongle_release_hold_actions();
    }

    /* Handle tap-like actions based on trigger mode */
    bool should_fire = false;

    switch (action->trigger) {
    case TRIGGER_COOLDOWN: {
        int64_t elapsed = now_us - s_dongle_last_fire_us[class_idx];
        if (elapsed >= ((int64_t)action->cooldown_ms * 1000LL)) {
            should_fire = true;
        } else {
            s_dongle_hid_status = "cooldown_wait";
        }
        break;
    }
    case TRIGGER_EDGE: {
        if (s_dongle_edge_armed[class_idx]) {
            should_fire = true;
        } else {
            s_dongle_hid_status = "edge_wait";
        }
        break;
    }
    case TRIGGER_SUSTAIN: {
        if (s_dongle_sustain_count[class_idx] == action->sustain_frames) {
            should_fire = true;
        } else {
            s_dongle_hid_status = "sustain_wait";
        }
        break;
    }
    case TRIGGER_REPEAT: {
        int64_t elapsed = now_us - s_dongle_last_fire_us[class_idx];
        if (s_dongle_sustain_count[class_idx] >= action->sustain_frames &&
            elapsed >= ((int64_t)action->cooldown_ms * 1000LL)) {
            should_fire = true;
        } else {
            s_dongle_hid_status = "repeat_wait";
        }
        break;
    }
    }

    if (should_fire) {
        dongle_fire_action(class_idx, action, now_us);
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

static void dongle_run_rf_inference(int64_t now_us)
{
    rf_infer_node_sample_t frame[RF_INFER_NODE_COUNT] = {0};
    double max_age_ms = 0.0;

    if (!dongle_build_rf_frame(frame, now_us, &max_age_ms)) {
        return;
    }

    rf_infer_result_t result = {0};
    const int64_t infer_start_us = esp_timer_get_time();
    if (!rf_infer_push_frame(frame, &result) || !result.valid) {
        return;
    }
    const int64_t infer_elapsed_us = esp_timer_get_time() - infer_start_us;

    const uint8_t result_class = result.class_index;
    const float result_confidence = result.confidence;
    const char *result_label = result.label;
    const uint32_t result_frames = result.frame_count;

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
    if (result_confidence < DONGLE_RF_MIN_CONFIDENCE) {
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
                dongle_store_latest_packet(&rx_packet);
            } else if (rx_packet.packet.type == M2P_ESPNOW_PACKET_BLADE_STATE) {
                dongle_store_blade_packet(&rx_packet);
            }
#endif
        }

#if DONGLE_ENABLE_SERIAL_OUTPUT
        now_tick = xTaskGetTickCount();
        if ((now_tick - last_print_tick) >= print_period_ticks) {
            dongle_print_latest_states();
#if DONGLE_ENABLE_RF_INFERENCE
            dongle_run_rf_inference(esp_timer_get_time());
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
    (void)blade_button_init();

    esp_err_t wake_err = blade_enable_deep_sleep_wakeup();
    if (wake_err != ESP_OK) {
        ESP_LOGE(TAG, "Blade deep sleep wakeup config failed: %s", esp_err_to_name(wake_err));
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
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
        vTaskDelay(pdMS_TO_TICKS(BLADE_REPORT_PERIOD_MS));
    }

    ESP_LOGI(TAG, "Blade wake hold confirmed, starting normal mode");
    printf("# blade-wake: confirmed=1\n");
}

typedef enum {
    BLADE_SLEEP_GESTURE_IDLE = 0,
    BLADE_SLEEP_GESTURE_WAIT_SECOND_PRESS,
    BLADE_SLEEP_GESTURE_SECOND_HOLD,
} blade_sleep_gesture_state_t;

static void blade_report_task(void *arg)
{
    (void)arg;

    TickType_t last_wake = xTaskGetTickCount();
    uint32_t sequence = 0;
    bool last_raw_pressed = blade_button_is_pressed();
    bool stable_pressed = last_raw_pressed;
    uint8_t same_sample_count = BLADE_DEBOUNCE_SAMPLES;
    blade_sleep_gesture_state_t sleep_gesture_state = BLADE_SLEEP_GESTURE_IDLE;
    TickType_t press_start_tick = last_wake;
    TickType_t first_click_release_tick = 0;
    TickType_t second_hold_start_tick = 0;
#if BLADE_ENABLE_SERIAL_OUTPUT
    TickType_t last_serial_tick = 0;
    bool force_serial_print = true;
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
                if (sleep_gesture_state == BLADE_SLEEP_GESTURE_WAIT_SECOND_PRESS &&
                    elapsed_ms_since(first_click_release_tick, now_tick) <= BLADE_SLEEP_DOUBLE_CLICK_GAP_MS) {
                    sleep_gesture_state = BLADE_SLEEP_GESTURE_SECOND_HOLD;
                    second_hold_start_tick = now_tick;
                    ESP_LOGI(TAG,
                             "Blade sleep gesture: second press detected, hold %d ms to sleep",
                             BLADE_SLEEP_HOLD_MS);
                    printf("# blade-sleep-gesture: stage=second_hold hold_required_ms=%d\n",
                           BLADE_SLEEP_HOLD_MS);
                }
            } else {
                const uint32_t press_ms = elapsed_ms_since(press_start_tick, now_tick);
                if (sleep_gesture_state == BLADE_SLEEP_GESTURE_SECOND_HOLD) {
                    sleep_gesture_state = BLADE_SLEEP_GESTURE_IDLE;
                    ESP_LOGI(TAG, "Blade sleep gesture cancelled before long hold");
                    printf("# blade-sleep-gesture: stage=cancelled press_ms=%" PRIu32 "\n",
                           press_ms);
                } else if (sleep_gesture_state == BLADE_SLEEP_GESTURE_IDLE &&
                           press_ms <= BLADE_SLEEP_FIRST_CLICK_MAX_MS) {
                    sleep_gesture_state = BLADE_SLEEP_GESTURE_WAIT_SECOND_PRESS;
                    first_click_release_tick = now_tick;
                    ESP_LOGI(TAG,
                             "Blade sleep gesture: first click detected, waiting %d ms for second press",
                             BLADE_SLEEP_DOUBLE_CLICK_GAP_MS);
                    printf("# blade-sleep-gesture: stage=first_click press_ms=%" PRIu32 "\n",
                           press_ms);
                }
            }
        }

        if (sleep_gesture_state == BLADE_SLEEP_GESTURE_WAIT_SECOND_PRESS &&
            elapsed_ms_since(first_click_release_tick, now_tick) > BLADE_SLEEP_DOUBLE_CLICK_GAP_MS) {
            sleep_gesture_state = BLADE_SLEEP_GESTURE_IDLE;
        }

        if (sleep_gesture_state == BLADE_SLEEP_GESTURE_SECOND_HOLD &&
            stable_pressed &&
            elapsed_ms_since(second_hold_start_tick, now_tick) >= BLADE_SLEEP_HOLD_MS) {
            blade_enter_deep_sleep("double_click_long_hold");
        }

        esp_err_t send_err = ESP_OK;
#if MOVE_TO_PLAY_ENABLE_ESPNOW
        send_err = m2p_espnow_send_blade_state(BLADE_NODE_ID, sequence, stable_pressed);
        if (send_err != ESP_OK &&
            (sequence % BLADE_REPORT_RATE_HZ) == 0) {
            ESP_LOGW(TAG,
                     "ESP-NOW blade state send failed: %s",
                     esp_err_to_name(send_err));
        }
#endif
#if BLADE_ENABLE_SERIAL_OUTPUT
        if (force_serial_print ||
            state_changed ||
            (now_tick - last_serial_tick) >= pdMS_TO_TICKS(BLADE_SERIAL_STATE_PERIOD_MS)) {
            printf("# blade-tx: node_id=%u pressed=%d raw_pressed=%d gpio=%d"
                   " seq=%" PRIu32 " send=%s\n",
                   BLADE_NODE_ID,
                   stable_pressed ? 1 : 0,
                   raw_pressed ? 1 : 0,
                   gpio_level,
                   sequence,
                   esp_err_to_name(send_err));
            last_serial_tick = now_tick;
            force_serial_print = false;
        }
#endif
        sequence++;

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(BLADE_REPORT_PERIOD_MS));
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
    ESP_LOGI(TAG, "report_rate_hz=%d", BLADE_REPORT_RATE_HZ);

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
        return;
    }
#else
    ESP_LOGI(TAG, "ESP-NOW disabled by MOVE_TO_PLAY_ENABLE_ESPNOW");
#endif

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

    battery_monitor_init(BATTERY_ADC_UNIT, BATTERY_ADC_CHANNEL, BATTERY_ADC_GPIO);
    vTaskDelay(pdMS_TO_TICKS(500));

    while (1) {
        int percent = battery_monitor_get_percent();
        float voltage = battery_monitor_get_voltage();

        printf("# battery: node_id=%d node_name=%s board_style=%d board_name=%s %.2fV  %d%%\n",
               BOARD_NODE_ID,
               tracker_node_name(BOARD_NODE_ID),
               MOVE_TO_PLAY_TRACKER_BOARD_STYLE,
               MOVE_TO_PLAY_TRACKER_BOARD_NAME,
               (double)voltage,
               percent);
        status_led_set_battery_color(percent);

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
        start_blade_mode();
    }
}
