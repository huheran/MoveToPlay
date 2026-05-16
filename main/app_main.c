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
#include "rf_infer.h"
#include "cnn_infer.h"
#include "cnn_infer_int8.h"
#include "usb_keyboard.h"
#include "battery_monitor.h"
#include "status_led.h"

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
 *
 * DONGLE_ENABLE_USB_MOUSE:
 *   0 = disable USB mouse report output.
 *   1 = enable TinyUSB HID mouse support on the same USB HID device.
 */
/*
 * [烧录前先改这里]
 *
 * 1) 烧 dongle（接收端）时：
 *    #define MOVE_TO_PLAY_DEVICE_MODE MOVE_TO_PLAY_MODE_DONGLE
 *
 * 2) 烧 tracker（身体节点）时：
 *    #define MOVE_TO_PLAY_DEVICE_MODE MOVE_TO_PLAY_MODE_TRACKER
 */
//#define MOVE_TO_PLAY_DEVICE_MODE      MOVE_TO_PLAY_MODE_TRACKER
#define MOVE_TO_PLAY_DEVICE_MODE      MOVE_TO_PLAY_MODE_DONGLE

#define MOVE_TO_PLAY_ENABLE_ESPNOW        1
#define MOVE_TO_PLAY_ESPNOW_SEND_SAMPLES  1

/*
 * DONGLE_DATA_COLLECT_MODE:
 *   1 = 数据采集模式。关闭推理和USB HID，只输出原始CSV供采集脚本使用。
 *   0 = 正常游玩模式。开启推理和USB HID输出。
 */
#define DONGLE_DATA_COLLECT_MODE          1

#if DONGLE_DATA_COLLECT_MODE
#define DONGLE_ENABLE_SERIAL_OUTPUT       1
#define DONGLE_ENABLE_RAW_CSV_OUTPUT      1
#define DONGLE_ENABLE_SERIAL_AGE_COLUMN   1
#define DONGLE_ENABLE_RF_INFERENCE        0
#define DONGLE_USE_CNN_INFER              0
#define DONGLE_USE_CNN_INT8               0
#define DONGLE_ENABLE_USB_KEYBOARD        0
#define DONGLE_ENABLE_USB_MOUSE           0
#define DONGLE_ENABLE_USB_KEYBOARD_TEST   0
#define DONGLE_ENABLE_USB_MOUSE_TEST      0
#else
#define DONGLE_ENABLE_SERIAL_OUTPUT       1
#define DONGLE_ENABLE_RAW_CSV_OUTPUT      0
#define DONGLE_ENABLE_SERIAL_AGE_COLUMN   1
#define DONGLE_ENABLE_RF_INFERENCE        1
#define DONGLE_USE_CNN_INFER              1
#define DONGLE_USE_CNN_INT8               1
#define DONGLE_ENABLE_USB_KEYBOARD        1
#define DONGLE_ENABLE_USB_MOUSE           1
#define DONGLE_ENABLE_USB_KEYBOARD_TEST   0
#define DONGLE_ENABLE_USB_MOUSE_TEST      0
#endif

#define TRACKER_NODE_CHEST            1
#define TRACKER_NODE_RIGHT_HAND       2
#define TRACKER_NODE_LEFT_HAND        3
#define TRACKER_NODE_LEG              4

/*
 * [如果上面选的是 TRACKER，再改这里]
 *
 * 给这块 tracker 板子分配身体位置编号。
 * 每烧一块 tracker，只改下面这一行即可：
 *
 *   TRACKER_NODE_CHEST       胸部
 *   TRACKER_NODE_RIGHT_HAND  右手
 *   TRACKER_NODE_LEFT_HAND   左手
 *   TRACKER_NODE_LEG         腿部
 *
 * 例子：
 *   胸部板     -> TRACKER_NODE_CHEST
 *   右手板     -> TRACKER_NODE_RIGHT_HAND
 *   左手板     -> TRACKER_NODE_LEFT_HAND
 *   腿部板     -> TRACKER_NODE_LEG
 */
#define MOVE_TO_PLAY_TRACKER_NODE_ID  TRACKER_NODE_LEFT_HAND

#define BOARD_NODE_ID                 MOVE_TO_PLAY_TRACKER_NODE_ID

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
#define DONGLE_RF_MAX_NODE_AGE_MS     250
#define DONGLE_RF_PRINT_INTERVAL_MS   120
#define DONGLE_RF_MIN_CONFIDENCE      0.45f

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
    (MOVE_TO_PLAY_DEVICE_MODE != MOVE_TO_PLAY_MODE_TRACKER)
#error "MOVE_TO_PLAY_DEVICE_MODE must be 0 (dongle) or 1 (tracker)"
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
    status_led_init();
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

static dongle_latest_node_t s_dongle_latest_nodes[DONGLE_MAX_TRACKER_NODES];
static int64_t s_dongle_last_rf_print_us = 0;

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
#if DONGLE_ENABLE_RAW_CSV_OUTPUT
    const int64_t now_us = esp_timer_get_time();

    for (size_t i = 0; i < DONGLE_MAX_TRACKER_NODES; i++) {
        dongle_latest_node_t *node = &s_dongle_latest_nodes[i];
        if (node->valid && node->dirty) {
            dongle_print_latest_node(node, now_us);
        }
    }
#endif
}

#if DONGLE_ENABLE_RF_INFERENCE

#if DONGLE_ENABLE_USB_KEYBOARD
#define DONGLE_KEY_TAP_HOLD_MS        80
#define DONGLE_MOUSE_MOVE_DELTA       60
#define DONGLE_CONFIRM_FRAMES         3
#define DONGLE_INFER_RATE_HZ          25

#define HID_KEY_E       0x08
#define HID_KEY_M       0x10
#define HID_KEY_Q       0x14
#define HID_KEY_W       0x1A
#define HID_KEY_SPACE   0x2C
#define HID_KEY_ESCAPE  0x29

typedef enum {
    ACTION_TYPE_NONE,
    ACTION_TYPE_KEY_TAP,
    ACTION_TYPE_KEY_HOLD,
    ACTION_TYPE_MOUSE_CLICK,
    ACTION_TYPE_MOUSE_MOVE_LEFT,
} dongle_action_type_t;

typedef enum {
    TRIGGER_COOLDOWN,
    TRIGGER_EDGE,
    TRIGGER_SUSTAIN,
} dongle_trigger_mode_t;

typedef struct {
    dongle_action_type_t type;
    uint8_t modifier;
    uint8_t keycode;
    dongle_trigger_mode_t trigger;
    uint16_t cooldown_ms;
    uint16_t sustain_frames;
} dongle_key_action_t;

#if DONGLE_USE_CNN_INFER
/* CNN class order: idle(0), right_hand_raise(1), right_hand_slash(2), run(3),
   walk(4), hands_cross_forehead(5), left_hand_raise(6), ultraman_beam(7),
   hands_press_down(8), kick(9), jump(10), turn_body(11), hands_shoot(12) */
#if DONGLE_USE_CNN_INT8
#define DONGLE_NUM_CLASSES CNN1D_INT8_NUM_CLASSES
#else
#define DONGLE_NUM_CLASSES CNN1D_NUM_CLASSES
#endif
#define DONGLE_IDLE_CLASS 0
static const dongle_key_action_t s_class_key_actions[DONGLE_NUM_CLASSES] = {
    [0]  = { ACTION_TYPE_NONE, 0, 0, TRIGGER_COOLDOWN, 0, 0 },                              /* idle */
    [1]  = { ACTION_TYPE_KEY_TAP, 0, HID_KEY_M, TRIGGER_SUSTAIN, 0, 25 },                   /* right_hand_raise -> M */
    [2]  = { ACTION_TYPE_MOUSE_CLICK, 0, 0, TRIGGER_COOLDOWN, 400, 0 },                     /* right_hand_slash -> 鼠标左键 */
    [3]  = { ACTION_TYPE_KEY_HOLD, USB_KEYBOARD_MOD_LEFT_SHIFT, HID_KEY_W, TRIGGER_COOLDOWN, 0, 0 }, /* run */
    [4]  = { ACTION_TYPE_KEY_HOLD, 0, HID_KEY_W, TRIGGER_COOLDOWN, 0, 0 },                  /* walk */
    [5]  = { ACTION_TYPE_KEY_TAP, 0, HID_KEY_E, TRIGGER_COOLDOWN, 1000, 0 },                /* hands_cross_forehead -> E */
    [6]  = { ACTION_TYPE_KEY_TAP, 0, HID_KEY_ESCAPE, TRIGGER_SUSTAIN, 0, 25 },              /* left_hand_raise -> ESC */
    [7]  = { ACTION_TYPE_KEY_TAP, 0, HID_KEY_Q, TRIGGER_COOLDOWN, 2000, 0 },                /* ultraman_beam -> Q */
    [8]  = { ACTION_TYPE_KEY_TAP, 0, HID_KEY_F, TRIGGER_COOLDOWN, 1000, 0 },                /* hands_press_down -> F */
    [9]  = { ACTION_TYPE_KEY_TAP, 0, HID_KEY_SPACE, TRIGGER_EDGE, 3000, 0 },                /* kick -> SPACE */
    [10] = { ACTION_TYPE_KEY_TAP, 0, HID_KEY_SPACE, TRIGGER_EDGE, 3000, 0 },                /* jump -> SPACE */
    [11] = { ACTION_TYPE_MOUSE_MOVE_LEFT, 0, 0, TRIGGER_SUSTAIN, 0, 25 },                   /* turn_body -> 鼠标左移 */
    [12] = { ACTION_TYPE_MOUSE_CLICK, 0, 0, TRIGGER_COOLDOWN, 400, 0 },                     /* hands_shoot -> 鼠标左键 */
};
#else
/* RF class order: both_hands_raise(0), hands_chest_push(1), hands_cross_chest(2),
   idle(3), jump(4), left_hand_raise(5), right_hand_raise(6),
   right_hand_slash(7), run(8), walk(9) */
static const dongle_key_action_t s_class_key_actions[RF_MODEL_CLASS_COUNT] = {
    [0] = { ACTION_TYPE_MOUSE_MOVE_LEFT, 0, 0, TRIGGER_SUSTAIN, 0, 25 },
    [1] = { ACTION_TYPE_KEY_TAP, 0, HID_KEY_Q, TRIGGER_COOLDOWN, 2000, 0 },
    [2] = { ACTION_TYPE_KEY_TAP, 0, HID_KEY_E, TRIGGER_COOLDOWN, 1000, 0 },
    [3] = { ACTION_TYPE_NONE, 0, 0, TRIGGER_COOLDOWN, 0, 0 },
    [4] = { ACTION_TYPE_KEY_TAP, 0, HID_KEY_SPACE, TRIGGER_EDGE, 3000, 0 },
    [5] = { ACTION_TYPE_KEY_TAP, 0, HID_KEY_ESCAPE, TRIGGER_SUSTAIN, 0, 25 },
    [6] = { ACTION_TYPE_KEY_TAP, 0, HID_KEY_M, TRIGGER_SUSTAIN, 0, 25 },
    [7] = { ACTION_TYPE_MOUSE_CLICK, 0, 0, TRIGGER_COOLDOWN, 400, 0 },
    [8] = { ACTION_TYPE_KEY_HOLD, USB_KEYBOARD_MOD_LEFT_SHIFT, HID_KEY_W, TRIGGER_COOLDOWN, 0, 0 },
    [9] = { ACTION_TYPE_KEY_HOLD, 0, HID_KEY_W, TRIGGER_COOLDOWN, 0, 0 },
};
#define DONGLE_NUM_CLASSES RF_MODEL_CLASS_COUNT
#define DONGLE_IDLE_CLASS 3
#endif

static int8_t s_dongle_held_class = -1;
static int8_t s_dongle_confirmed_class = -1;
static int8_t s_dongle_pending_class = -1;
static uint8_t s_dongle_pending_count = 0;

static int64_t s_dongle_last_fire_us[DONGLE_NUM_CLASSES] = {0};
static bool s_dongle_edge_armed[DONGLE_NUM_CLASSES] = {
    true, true, true, true, true, true, true, true, true, true, true, true, true
};
static uint16_t s_dongle_sustain_count[DONGLE_NUM_CLASSES] = {0};

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

static void dongle_fire_action(const dongle_key_action_t *action)
{
    if (action->type == ACTION_TYPE_KEY_TAP) {
        usb_keyboard_tap_key(action->modifier, action->keycode, DONGLE_KEY_TAP_HOLD_MS);
    } else if (action->type == ACTION_TYPE_MOUSE_CLICK) {
        usb_mouse_click(USB_MOUSE_BUTTON_LEFT, DONGLE_KEY_TAP_HOLD_MS);
    } else if (action->type == ACTION_TYPE_MOUSE_MOVE_LEFT) {
        usb_mouse_move(-DONGLE_MOUSE_MOVE_DELTA, 0, 0, 0);
    }
}

static void dongle_send_key_action(uint8_t infer_class, float infer_confidence, int64_t now_us)
{
    if (!usb_keyboard_is_ready()) {
        return;
    }

    uint8_t raw_class = infer_class;
    if (infer_confidence < DONGLE_RF_MIN_CONFIDENCE) {
        raw_class = DONGLE_IDLE_CLASS;
    }

    uint8_t class_idx = dongle_smooth_class(raw_class);
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
        if (s_dongle_held_class >= 0) {
            usb_keyboard_release();
            s_dongle_held_class = -1;
        }
        return;
    }

    /* Handle HOLD (walk/run) */
    if (action->type == ACTION_TYPE_KEY_HOLD) {
        if (s_dongle_held_class != (int8_t)class_idx) {
            if (s_dongle_held_class >= 0) {
                usb_keyboard_release();
            }
            const uint8_t keycodes[6] = { action->keycode, 0, 0, 0, 0, 0 };
            usb_keyboard_press_keys(action->modifier, keycodes);
            s_dongle_held_class = (int8_t)class_idx;
        }
        return;
    }

    /* Release any held key before tap actions */
    if (s_dongle_held_class >= 0) {
        usb_keyboard_release();
        s_dongle_held_class = -1;
    }

    /* Handle tap-like actions based on trigger mode */
    bool should_fire = false;

    switch (action->trigger) {
    case TRIGGER_COOLDOWN: {
        int64_t elapsed = now_us - s_dongle_last_fire_us[class_idx];
        if (elapsed >= ((int64_t)action->cooldown_ms * 1000LL)) {
            should_fire = true;
        }
        break;
    }
    case TRIGGER_EDGE: {
        if (s_dongle_edge_armed[class_idx]) {
            should_fire = true;
        }
        break;
    }
    case TRIGGER_SUSTAIN: {
        if (s_dongle_sustain_count[class_idx] == action->sustain_frames) {
            should_fire = true;
        }
        break;
    }
    }

    if (should_fire) {
        dongle_fire_action(action);
        s_dongle_last_fire_us[class_idx] = now_us;
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

#if DONGLE_USE_CNN_INFER
#if DONGLE_USE_CNN_INT8
    cnn_int8_infer_node_sample_t cnn_frame[CNN_INT8_INFER_NODE_COUNT];
    for (int i = 0; i < CNN_INT8_INFER_NODE_COUNT; i++) {
        cnn_frame[i].ax = frame[i].ax;
        cnn_frame[i].ay = frame[i].ay;
        cnn_frame[i].az = frame[i].az;
        cnn_frame[i].gx = frame[i].gx;
        cnn_frame[i].gy = frame[i].gy;
        cnn_frame[i].gz = frame[i].gz;
    }

    cnn_int8_infer_result_t cnn_result = {0};
    const int64_t infer_start_us = esp_timer_get_time();
    if (!cnn_int8_infer_push_frame(cnn_frame, &cnn_result) || !cnn_result.valid) {
        return;
    }
    const int64_t infer_elapsed_us = esp_timer_get_time() - infer_start_us;

    const uint8_t result_class = cnn_result.class_index;
    const float result_confidence = cnn_result.confidence;
    const char *result_label = cnn_result.label;
    const uint32_t result_frames = cnn_result.frame_count;
#else
    cnn_infer_node_sample_t cnn_frame[CNN_INFER_NODE_COUNT];
    for (int i = 0; i < CNN_INFER_NODE_COUNT; i++) {
        cnn_frame[i].ax = frame[i].ax;
        cnn_frame[i].ay = frame[i].ay;
        cnn_frame[i].az = frame[i].az;
        cnn_frame[i].gx = frame[i].gx;
        cnn_frame[i].gy = frame[i].gy;
        cnn_frame[i].gz = frame[i].gz;
    }

    cnn_infer_result_t cnn_result = {0};
    const int64_t infer_start_us = esp_timer_get_time();
    if (!cnn_infer_push_frame(cnn_frame, &cnn_result) || !cnn_result.valid) {
        return;
    }
    const int64_t infer_elapsed_us = esp_timer_get_time() - infer_start_us;

    const uint8_t result_class = cnn_result.class_index;
    const float result_confidence = cnn_result.confidence;
    const char *result_label = cnn_result.label;
    const uint32_t result_frames = cnn_result.frame_count;
#endif
#else
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
#endif

#if DONGLE_ENABLE_USB_KEYBOARD
    dongle_send_key_action(result_class, result_confidence, now_us);
#endif

    if ((now_us - s_dongle_last_rf_print_us) <
        ((int64_t)DONGLE_RF_PRINT_INTERVAL_MS * 1000LL)) {
        return;
    }
    s_dongle_last_rf_print_us = now_us;

    const char *display_label = result_label;
    if (result_confidence < DONGLE_RF_MIN_CONFIDENCE) {
        display_label = "uncertain";
    }

    printf("# infer: action=%s conf=%.2f frames=%" PRIu32 " max_age_ms=%.1f infer_us=%" PRId64 "\n",
           display_label,
           (double)result_confidence,
           result_frames,
           max_age_ms,
           infer_elapsed_us);
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

static void battery_monitor_task(void *arg)
{
    (void)arg;

    battery_monitor_init();
    vTaskDelay(pdMS_TO_TICKS(500));

    while (1) {
        int percent = battery_monitor_get_percent();
        float voltage = battery_monitor_get_voltage();

        printf("# battery: %.2fV  %d%%\n", (double)voltage, percent);
        status_led_set_battery_color(percent);

        vTaskDelay(pdMS_TO_TICKS(BATTERY_REPORT_INTERVAL_MS));
    }
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
        xTaskCreatePinnedToCore(battery_monitor_task,
                                "battery_task",
                                3072,
                                NULL,
                                3,
                                NULL,
                                tskNO_AFFINITY);
        start_tracker_mode();
    }
}
