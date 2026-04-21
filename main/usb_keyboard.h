#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_KEYBOARD_MOD_LEFT_CTRL   0x01
#define USB_KEYBOARD_MOD_LEFT_SHIFT  0x02
#define USB_KEYBOARD_MOD_LEFT_ALT    0x04
#define USB_KEYBOARD_MOD_LEFT_GUI    0x08
#define USB_KEYBOARD_MOD_RIGHT_CTRL  0x10
#define USB_KEYBOARD_MOD_RIGHT_SHIFT 0x20
#define USB_KEYBOARD_MOD_RIGHT_ALT   0x40
#define USB_KEYBOARD_MOD_RIGHT_GUI   0x80

typedef enum {
    USB_KEYBOARD_KEY_NONE = 0x00,
    USB_KEYBOARD_KEY_A = 0x04,
    USB_KEYBOARD_KEY_D = 0x07,
    USB_KEYBOARD_KEY_S = 0x16,
    USB_KEYBOARD_KEY_W = 0x1A,
    USB_KEYBOARD_KEY_ENTER = 0x28,
    USB_KEYBOARD_KEY_ESCAPE = 0x29,
    USB_KEYBOARD_KEY_SPACE = 0x2C,
    USB_KEYBOARD_KEY_RIGHT = 0x4F,
    USB_KEYBOARD_KEY_LEFT = 0x50,
    USB_KEYBOARD_KEY_DOWN = 0x51,
    USB_KEYBOARD_KEY_UP = 0x52,
} usb_keyboard_key_t;

#define USB_MOUSE_BUTTON_LEFT    0x01
#define USB_MOUSE_BUTTON_RIGHT   0x02
#define USB_MOUSE_BUTTON_MIDDLE  0x04
#define USB_MOUSE_BUTTON_BACK    0x08
#define USB_MOUSE_BUTTON_FORWARD 0x10

esp_err_t usb_keyboard_init(void);
bool usb_keyboard_is_ready(void);
esp_err_t usb_keyboard_press_keys(uint8_t modifier, const uint8_t keycodes[6]);
esp_err_t usb_keyboard_release(void);
esp_err_t usb_keyboard_tap_key(uint8_t modifier, uint8_t keycode, uint32_t hold_ms);
bool usb_mouse_is_ready(void);
esp_err_t usb_mouse_report(uint8_t buttons, int8_t x, int8_t y, int8_t wheel, int8_t pan);
esp_err_t usb_mouse_move(int8_t x, int8_t y, int8_t wheel, int8_t pan);
esp_err_t usb_mouse_set_buttons(uint8_t buttons);
esp_err_t usb_mouse_release_buttons(void);
esp_err_t usb_mouse_click(uint8_t buttons, uint32_t hold_ms);

#ifdef __cplusplus
}
#endif
