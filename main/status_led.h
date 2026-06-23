#pragma once

#include "driver/gpio.h"
#include "esp_err.h"
#include <stdint.h>

esp_err_t status_led_init(gpio_num_t data_gpio);
void status_led_set_color(uint8_t r, uint8_t g, uint8_t b);
void status_led_off(void);
void status_led_set_battery_color(int percent);
