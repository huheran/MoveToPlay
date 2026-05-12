#pragma once

#include "esp_err.h"
#include <stdint.h>

esp_err_t battery_monitor_init(void);
int battery_monitor_get_percent(void);
float battery_monitor_get_voltage(void);
