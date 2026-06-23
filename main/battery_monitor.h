#pragma once

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include <stdint.h>

esp_err_t battery_monitor_init(adc_unit_t unit_id, adc_channel_t channel, gpio_num_t sense_gpio);
int battery_monitor_get_percent(void);
float battery_monitor_get_voltage(void);
