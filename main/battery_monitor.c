#include "battery_monitor.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static const char *TAG = "battery";

#define BATTERY_ADC_ATTEN       ADC_ATTEN_DB_12

#define VOLTAGE_DIVIDER_RATIO   2.0f

#define BATTERY_FULL_MV         4200
#define BATTERY_EMPTY_MV        3000

static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_cali_handle;
static bool s_cali_valid;
static adc_unit_t s_adc_unit;
static adc_channel_t s_adc_channel;
static gpio_num_t s_battery_gpio;

esp_err_t battery_monitor_init(adc_unit_t unit_id, adc_channel_t channel, gpio_num_t sense_gpio)
{
    s_adc_unit = unit_id;
    s_adc_channel = channel;
    s_battery_gpio = sense_gpio;

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = s_adc_unit,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_12,
    };
    err = adc_oneshot_config_channel(s_adc_handle, s_adc_channel, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = s_adc_unit,
        .chan = s_adc_channel,
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_12,
    };
    err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle);
    s_cali_valid = (err == ESP_OK);
    if (!s_cali_valid) {
        ESP_LOGW(TAG, "ADC calibration not available, using raw values");
    }

    ESP_LOGI(TAG,
             "battery monitor initialized on GPIO%d (ADC unit=%d channel=%d)",
             s_battery_gpio,
             s_adc_unit,
             s_adc_channel);
    return ESP_OK;
}

float battery_monitor_get_voltage(void)
{
    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_adc_handle, s_adc_channel, &raw);
    if (err != ESP_OK) {
        return 0.0f;
    }

    int voltage_mv = 0;
    if (s_cali_valid) {
        adc_cali_raw_to_voltage(s_cali_handle, raw, &voltage_mv);
    } else {
        voltage_mv = (raw * 3300) / 4095;
    }

    float vbat_mv = (float)voltage_mv * VOLTAGE_DIVIDER_RATIO;
    return vbat_mv / 1000.0f;
}

int battery_monitor_get_percent(void)
{
    float vbat_mv = battery_monitor_get_voltage() * 1000.0f;

    if (vbat_mv >= BATTERY_FULL_MV) {
        return 100;
    }
    if (vbat_mv <= BATTERY_EMPTY_MV) {
        return 0;
    }

    return (int)((vbat_mv - BATTERY_EMPTY_MV) * 100.0f / (BATTERY_FULL_MV - BATTERY_EMPTY_MV));
}
