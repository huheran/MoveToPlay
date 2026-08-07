#include "usb_keyboard.h"

#include <stddef.h>
#include <string.h>

#include "class/cdc/cdc_device.h"
#include "class/hid/hid_device.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"

static const char *TAG = "usb_keyboard";

#define USB_REPORT_ID_KEYBOARD 1
#define USB_REPORT_ID_MOUSE    2
#define USB_KEYBOARD_TOTAL_DESC_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)
#define USB_HID_EP_SIZE 16
#define USB_HID_EP_INTERVAL_MS 1
#define USB_HID_SEND_TIMEOUT_MS 100
#define USB_HID_SEND_RETRY_MS   2
#define USB_TELEMETRY_MAX_LINE_SIZE 500

static bool s_usb_keyboard_installed = false;
static bool s_usb_serial_jtag_mode = false;
static volatile bool s_usb_telemetry_port_open = false;
static uint8_t s_mouse_buttons = 0;

static const uint8_t s_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(USB_REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(USB_REPORT_ID_MOUSE)),
};

static const char *s_hid_string_descriptor[] = {
    (const char[]){0x09, 0x04},
    "MoveToPlay",
    "MoveToPlay HID + Telemetry",
    "000003",
    "HID Keyboard+Mouse",
    "MoveToPlay Telemetry",
};

static const tusb_desc_device_t s_composite_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = TINYUSB_ESPRESSIF_VID,
    .idProduct = 0x4005,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const uint8_t s_hid_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, USB_KEYBOARD_TOTAL_DESC_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(0, 5, 0x81, 8, 0x02, 0x82, 64),
    TUD_HID_DESCRIPTOR(2, 4, HID_ITF_PROTOCOL_NONE, sizeof(s_hid_report_descriptor), 0x83, USB_HID_EP_SIZE, USB_HID_EP_INTERVAL_MS),
};

static void usb_telemetry_line_state_changed(int itf, cdcacm_event_t *event)
{
    (void)itf;
    s_usb_telemetry_port_open = event != NULL && event->line_state_changed_data.dtr;
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance,
                               uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer,
                               uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;

    return 0;
}

void tud_hid_set_report_cb(uint8_t instance,
                           uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer,
                           uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

esp_err_t usb_keyboard_init(void)
{
    if (s_usb_keyboard_installed) {
        return ESP_OK;
    }

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();

    tusb_cfg.descriptor.device = &s_composite_device_descriptor;
    tusb_cfg.descriptor.full_speed_config = s_hid_configuration_descriptor;
    tusb_cfg.descriptor.string = s_hid_string_descriptor;
    tusb_cfg.descriptor.string_count = sizeof(s_hid_string_descriptor) / sizeof(s_hid_string_descriptor[0]);
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = s_hid_configuration_descriptor;
#endif

    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        return err;
    }

    const tinyusb_config_cdcacm_t cdc_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = NULL,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = usb_telemetry_line_state_changed,
        .callback_line_coding_changed = NULL,
    };
    err = tinyusb_cdcacm_init(&cdc_cfg);
    if (err != ESP_OK) {
        (void)tinyusb_driver_uninstall();
        return err;
    }

    s_usb_keyboard_installed = true;
    s_usb_telemetry_port_open = false;
    s_mouse_buttons = 0;
    ESP_LOGI(TAG, "USB HID keyboard+mouse and CDC telemetry initialized");
    return ESP_OK;
}

esp_err_t usb_keyboard_switch_to_serial_jtag(void)
{
    if (s_usb_serial_jtag_mode) {
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(s_usb_keyboard_installed,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "USB HID is not installed");
    ESP_RETURN_ON_FALSE(!usb_serial_jtag_is_driver_installed(),
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "USB Serial/JTAG driver is already installed");

    if (usb_keyboard_is_ready()) {
        esp_err_t keyboard_err = usb_keyboard_release();
        if (keyboard_err != ESP_OK) {
            ESP_LOGW(TAG, "Release keyboard before USB mode switch failed: %s",
                     esp_err_to_name(keyboard_err));
        }

        esp_err_t mouse_err = usb_mouse_release_buttons();
        if (mouse_err != ESP_OK) {
            ESP_LOGW(TAG, "Release mouse before USB mode switch failed: %s",
                     esp_err_to_name(mouse_err));
        }

        /* Allow the host to consume the final all-released HID reports. */
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0)) {
        esp_err_t cdc_err = tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0);
        if (cdc_err != ESP_OK) {
            ESP_LOGW(TAG, "Deinitialize USB telemetry CDC failed: %s", esp_err_to_name(cdc_err));
        }
    }
    s_usb_telemetry_port_open = false;

    esp_err_t err = tinyusb_driver_uninstall();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Uninstall TinyUSB HID failed: %s", esp_err_to_name(err));
        return err;
    }

    s_usb_keyboard_installed = false;
    s_mouse_buttons = 0;

    usb_serial_jtag_driver_config_t serial_jtag_cfg =
        USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    err = usb_serial_jtag_driver_install(&serial_jtag_cfg);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Enable USB Serial/JTAG failed: %s; restoring HID",
                 esp_err_to_name(err));
        esp_err_t restore_err = usb_keyboard_init();
        if (restore_err != ESP_OK) {
            ESP_LOGE(TAG, "Restore USB HID failed: %s", esp_err_to_name(restore_err));
        }
        return err;
    }

    s_usb_serial_jtag_mode = true;
    ESP_LOGI(TAG, "USB switched from HID keyboard/mouse to hardware Serial/JTAG");
    return ESP_OK;
}

bool usb_keyboard_is_ready(void)
{
    return s_usb_keyboard_installed && tud_mounted() && tud_hid_ready();
}

bool usb_telemetry_is_ready(void)
{
    return s_usb_keyboard_installed &&
           tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0) &&
           tud_mounted() &&
           s_usb_telemetry_port_open;
}

esp_err_t usb_telemetry_write_line(const char *line)
{
    ESP_RETURN_ON_FALSE(line != NULL, ESP_ERR_INVALID_ARG, TAG, "Telemetry line is NULL");
    ESP_RETURN_ON_FALSE(usb_telemetry_is_ready(), ESP_ERR_INVALID_STATE, TAG, "USB telemetry not ready");

    const size_t length = strlen(line);
    ESP_RETURN_ON_FALSE(length > 0 && length <= USB_TELEMETRY_MAX_LINE_SIZE,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "Telemetry line has invalid length");
    ESP_RETURN_ON_FALSE(tud_cdc_n_write_available(TINYUSB_CDC_ACM_0) >= length,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "USB telemetry TX queue is full");

    const size_t queued = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0,
                                                      (const uint8_t *)line,
                                                      length);
    if (queued != length) {
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t flush_err = tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    return (flush_err == ESP_OK || flush_err == ESP_ERR_NOT_FINISHED) ? ESP_OK : flush_err;
}

static esp_err_t usb_hid_send_keyboard_report(uint8_t modifier, const uint8_t keycodes[6])
{
    const TickType_t timeout_ticks = pdMS_TO_TICKS(USB_HID_SEND_TIMEOUT_MS);
    const TickType_t retry_ticks = pdMS_TO_TICKS(USB_HID_SEND_RETRY_MS);
    const TickType_t start_tick = xTaskGetTickCount();

    while (1) {
        if (usb_keyboard_is_ready() && tud_hid_keyboard_report(USB_REPORT_ID_KEYBOARD, modifier, keycodes)) {
            return ESP_OK;
        }

        if ((xTaskGetTickCount() - start_tick) >= timeout_ticks) {
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(retry_ticks > 0 ? retry_ticks : 1);
    }
}

static esp_err_t usb_hid_send_mouse_report(uint8_t buttons, int8_t x, int8_t y, int8_t wheel, int8_t pan)
{
    const TickType_t timeout_ticks = pdMS_TO_TICKS(USB_HID_SEND_TIMEOUT_MS);
    const TickType_t retry_ticks = pdMS_TO_TICKS(USB_HID_SEND_RETRY_MS);
    const TickType_t start_tick = xTaskGetTickCount();

    while (1) {
        if (usb_mouse_is_ready() && tud_hid_mouse_report(USB_REPORT_ID_MOUSE, buttons, x, y, wheel, pan)) {
            s_mouse_buttons = buttons;
            return ESP_OK;
        }

        if ((xTaskGetTickCount() - start_tick) >= timeout_ticks) {
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(retry_ticks > 0 ? retry_ticks : 1);
    }
}

esp_err_t usb_keyboard_press_keys(uint8_t modifier, const uint8_t keycodes[6])
{
    ESP_RETURN_ON_FALSE(usb_keyboard_is_ready(), ESP_ERR_INVALID_STATE, TAG, "USB keyboard not ready");
    return usb_hid_send_keyboard_report(modifier, keycodes);
}

esp_err_t usb_keyboard_release(void)
{
    ESP_RETURN_ON_FALSE(usb_keyboard_is_ready(), ESP_ERR_INVALID_STATE, TAG, "USB keyboard not ready");
    return usb_hid_send_keyboard_report(0, NULL);
}

esp_err_t usb_keyboard_tap_key(uint8_t modifier, uint8_t keycode, uint32_t hold_ms)
{
    uint8_t keycodes[6] = {keycode};
    esp_err_t err = usb_keyboard_press_keys(modifier, keycodes);
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    return usb_keyboard_release();
}

bool usb_mouse_is_ready(void)
{
    return usb_keyboard_is_ready();
}

esp_err_t usb_mouse_report(uint8_t buttons, int8_t x, int8_t y, int8_t wheel, int8_t pan)
{
    ESP_RETURN_ON_FALSE(usb_mouse_is_ready(), ESP_ERR_INVALID_STATE, TAG, "USB mouse not ready");
    return usb_hid_send_mouse_report(buttons, x, y, wheel, pan);
}

esp_err_t usb_mouse_move(int8_t x, int8_t y, int8_t wheel, int8_t pan)
{
    return usb_mouse_report(s_mouse_buttons, x, y, wheel, pan);
}

esp_err_t usb_mouse_set_buttons(uint8_t buttons)
{
    return usb_mouse_report(buttons, 0, 0, 0, 0);
}

esp_err_t usb_mouse_release_buttons(void)
{
    return usb_mouse_set_buttons(0);
}

esp_err_t usb_mouse_click(uint8_t buttons, uint32_t hold_ms)
{
    esp_err_t err = usb_mouse_set_buttons(buttons);
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    return usb_mouse_release_buttons();
}
