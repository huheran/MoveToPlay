#pragma once

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

typedef struct {
    uint32_t ir;
    uint32_t red;
} max30102_sample_t;

/* Initialize the MAX30102 on the Blade heart-rate connector. */
esp_err_t max30102_init(gpio_num_t sda_gpio, gpio_num_t scl_gpio, gpio_num_t int_gpio);

/* Put the optical front end into its low-power shutdown state. */
esp_err_t max30102_shutdown(void);

/* Read one sample from the FIFO. ESP_ERR_NOT_FOUND means the FIFO is empty. */
esp_err_t max30102_read_sample(max30102_sample_t *sample);
