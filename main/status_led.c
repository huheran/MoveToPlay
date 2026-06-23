#include "status_led.h"

#include "driver/rmt_tx.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "status_led";

#define RMT_RESOLUTION_HZ   10000000

#define SK6812_T0H_NS  300
#define SK6812_T0L_NS  900
#define SK6812_T1H_NS  600
#define SK6812_T1L_NS  600
#define SK6812_RESET_NS 80000

static rmt_channel_handle_t s_rmt_channel;
static rmt_encoder_handle_t s_encoder;

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} sk6812_encoder_t;

static size_t sk6812_encode(rmt_encoder_t *encoder,
                            rmt_channel_handle_t channel,
                            const void *primary_data,
                            size_t data_size,
                            rmt_encode_state_t *ret_state)
{
    sk6812_encoder_t *led_encoder = __containerof(encoder, sk6812_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;

    switch (led_encoder->state) {
    case 0:
        encoded_symbols += led_encoder->bytes_encoder->encode(
            led_encoder->bytes_encoder, channel, primary_data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_encoder->state = 1;
            session_state = RMT_ENCODING_RESET;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            *ret_state = (rmt_encode_state_t)session_state;
            return encoded_symbols;
        }
        /* fall through */
    case 1:
        encoded_symbols += led_encoder->copy_encoder->encode(
            led_encoder->copy_encoder, channel, &led_encoder->reset_code,
            sizeof(led_encoder->reset_code), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_encoder->state = RMT_ENCODING_RESET;
            *ret_state = RMT_ENCODING_COMPLETE;
        } else {
            *ret_state = (rmt_encode_state_t)session_state;
        }
        break;
    }
    return encoded_symbols;
}

static esp_err_t sk6812_del(rmt_encoder_t *encoder)
{
    sk6812_encoder_t *led_encoder = __containerof(encoder, sk6812_encoder_t, base);
    rmt_del_encoder(led_encoder->bytes_encoder);
    rmt_del_encoder(led_encoder->copy_encoder);
    free(led_encoder);
    return ESP_OK;
}

static esp_err_t sk6812_reset(rmt_encoder_t *encoder)
{
    sk6812_encoder_t *led_encoder = __containerof(encoder, sk6812_encoder_t, base);
    rmt_encoder_reset(led_encoder->bytes_encoder);
    rmt_encoder_reset(led_encoder->copy_encoder);
    led_encoder->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

static esp_err_t sk6812_encoder_new(rmt_encoder_handle_t *ret_encoder)
{
    sk6812_encoder_t *led_encoder = calloc(1, sizeof(sk6812_encoder_t));
    if (!led_encoder) {
        return ESP_ERR_NO_MEM;
    }

    led_encoder->base.encode = sk6812_encode;
    led_encoder->base.del = sk6812_del;
    led_encoder->base.reset = sk6812_reset;

    uint32_t ticks_per_ns = RMT_RESOLUTION_HZ / 1000000;

    rmt_bytes_encoder_config_t bytes_cfg = {
        .bit0 = {
            .level0 = 1,
            .duration0 = SK6812_T0H_NS * ticks_per_ns / 1000,
            .level1 = 0,
            .duration1 = SK6812_T0L_NS * ticks_per_ns / 1000,
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = SK6812_T1H_NS * ticks_per_ns / 1000,
            .level1 = 0,
            .duration1 = SK6812_T1L_NS * ticks_per_ns / 1000,
        },
        .flags.msb_first = 1,
    };
    esp_err_t err = rmt_new_bytes_encoder(&bytes_cfg, &led_encoder->bytes_encoder);
    if (err != ESP_OK) {
        free(led_encoder);
        return err;
    }

    rmt_copy_encoder_config_t copy_cfg = {};
    err = rmt_new_copy_encoder(&copy_cfg, &led_encoder->copy_encoder);
    if (err != ESP_OK) {
        rmt_del_encoder(led_encoder->bytes_encoder);
        free(led_encoder);
        return err;
    }

    led_encoder->reset_code = (rmt_symbol_word_t){
        .level0 = 0,
        .duration0 = SK6812_RESET_NS * ticks_per_ns / 1000 / 2,
        .level1 = 0,
        .duration1 = SK6812_RESET_NS * ticks_per_ns / 1000 / 2,
    };

    *ret_encoder = &led_encoder->base;
    return ESP_OK;
}

esp_err_t status_led_init(gpio_num_t data_gpio)
{
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = data_gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &s_rmt_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    err = sk6812_encoder_new(&s_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sk6812_encoder_new failed: %s", esp_err_to_name(err));
        return err;
    }

    err = rmt_enable(s_rmt_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    status_led_off();
    ESP_LOGI(TAG, "SK6812 LED initialized on GPIO%d", data_gpio);
    return ESP_OK;
}

void status_led_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t grb[3] = {g, r, b};
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    rmt_transmit(s_rmt_channel, s_encoder, grb, sizeof(grb), &tx_config);
    rmt_tx_wait_all_done(s_rmt_channel, 100);
}

void status_led_off(void)
{
    status_led_set_color(0, 0, 0);
}

void status_led_set_battery_color(int percent)
{
    if (percent > 75) {
        status_led_set_color(0, 20, 0);
    } else if (percent > 50) {
        status_led_set_color(20, 20, 0);
    } else if (percent > 25) {
        status_led_set_color(20, 10, 0);
    } else {
        status_led_set_color(20, 0, 0);
    }
}
