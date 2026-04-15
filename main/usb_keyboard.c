#include "usb_keyboard.h"

#include <stddef.h>

#include "class/hid/hid_device.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

static const char *TAG = "usb_keyboard";

#define USB_KEYBOARD_REPORT_ID 0
#define USB_KEYBOARD_TOTAL_DESC_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

static bool s_usb_keyboard_installed = false;

static const uint8_t s_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(),
};

static const char *s_hid_string_descriptor[] = {
    (const char[]){0x09, 0x04},
    "MoveToPlay",
    "MoveToPlay Keyboard",
    "000001",
    "HID Keyboard",
};

static const uint8_t s_hid_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, USB_KEYBOARD_TOTAL_DESC_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_KEYBOARD, sizeof(s_hid_report_descriptor), 0x81, 8, 10),
};

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

    tusb_cfg.descriptor.device = NULL;
    tusb_cfg.descriptor.full_speed_config = s_hid_configuration_descriptor;
    tusb_cfg.descriptor.string = s_hid_string_descriptor;
    tusb_cfg.descriptor.string_count = sizeof(s_hid_string_descriptor) / sizeof(s_hid_string_descriptor[0]);
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = s_hid_configuration_descriptor;
#endif

    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err == ESP_OK) {
        s_usb_keyboard_installed = true;
        ESP_LOGI(TAG, "USB HID keyboard initialized");
    }
    return err;
}

bool usb_keyboard_is_ready(void)
{
    return s_usb_keyboard_installed && tud_mounted() && tud_hid_ready();
}

esp_err_t usb_keyboard_press_keys(uint8_t modifier, const uint8_t keycodes[6])
{
    ESP_RETURN_ON_FALSE(usb_keyboard_is_ready(), ESP_ERR_INVALID_STATE, TAG, "USB keyboard not ready");

    if (!tud_hid_keyboard_report(USB_KEYBOARD_REPORT_ID, modifier, keycodes)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t usb_keyboard_release(void)
{
    ESP_RETURN_ON_FALSE(usb_keyboard_is_ready(), ESP_ERR_INVALID_STATE, TAG, "USB keyboard not ready");

    if (!tud_hid_keyboard_report(USB_KEYBOARD_REPORT_ID, 0, NULL)) {
        return ESP_FAIL;
    }
    return ESP_OK;
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
