#include "max30102.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "max30102";

#define MAX30102_I2C_ADDRESS       0x57
#define MAX30102_I2C_PORT          I2C_NUM_0
#define MAX30102_I2C_SPEED_HZ      400000
#define MAX30102_I2C_TIMEOUT_MS    100

#define REG_INTR_ENABLE_1          0x02
#define REG_INTR_ENABLE_2          0x03
#define REG_FIFO_WR_PTR            0x04
#define REG_OVF_COUNTER            0x05
#define REG_FIFO_RD_PTR            0x06
#define REG_FIFO_DATA              0x07
#define REG_FIFO_CONFIG             0x08
#define REG_MODE_CONFIG             0x09
#define REG_SPO2_CONFIG             0x0A
#define REG_LED1_PA                0x0C
#define REG_LED2_PA                0x0D
#define REG_REV_ID                 0xFE
#define REG_PART_ID                0xFF

#define MAX30102_PART_ID           0x15
#define MAX30102_FIFO_SIZE         32

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_initialized;

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t tx[] = {reg, value};
    return i2c_master_transmit(s_dev, tx, sizeof(tx), MAX30102_I2C_TIMEOUT_MS);
}

static esp_err_t read_regs(uint8_t reg, uint8_t *data, size_t length)
{
    return i2c_master_transmit_receive(s_dev,
                                       &reg,
                                       1,
                                       data,
                                       length,
                                       MAX30102_I2C_TIMEOUT_MS);
}

esp_err_t max30102_init(gpio_num_t sda_gpio, gpio_num_t scl_gpio, gpio_num_t int_gpio)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (int_gpio >= 0) {
        gpio_config_t int_config = {
            .pin_bit_mask = 1ULL << (uint32_t)int_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t gpio_err = gpio_config(&int_config);
        if (gpio_err != ESP_OK) {
            return gpio_err;
        }
    }

    i2c_master_bus_config_t bus_config = {
        .i2c_port = MAX30102_I2C_PORT,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = false,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MAX30102_I2C_ADDRESS,
        .scl_speed_hz = MAX30102_I2C_SPEED_HZ,
        .scl_wait_us = 0,
        .flags.disable_ack_check = false,
    };
    err = i2c_master_bus_add_device(s_bus, &device_config, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C device init failed: %s", esp_err_to_name(err));
        (void)i2c_del_master_bus(s_bus);
        s_bus = NULL;
        return err;
    }

    uint8_t part_id = 0;
    err = read_regs(REG_PART_ID, &part_id, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MAX30102 probe failed: %s", esp_err_to_name(err));
        (void)i2c_master_bus_rm_device(s_dev);
        (void)i2c_del_master_bus(s_bus);
        s_dev = NULL;
        s_bus = NULL;
        return err;
    }
    if (part_id != MAX30102_PART_ID) {
        ESP_LOGE(TAG, "unexpected part ID 0x%02X (expected 0x%02X)", part_id, MAX30102_PART_ID);
        (void)i2c_master_bus_rm_device(s_dev);
        (void)i2c_del_master_bus(s_bus);
        s_dev = NULL;
        s_bus = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t revision = 0;
    (void)read_regs(REG_REV_ID, &revision, 1);

    /* Reset, then configure SpO2 mode: 100 sps, 18-bit ADC, 411 us pulse. */
    err = write_reg(REG_MODE_CONFIG, 0x40);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    /* Assert INT (GPIO7) when a new averaged PPG sample is ready. The task
     * still drains the FIFO by polling so a missed edge cannot lose data. */
    err = write_reg(REG_INTR_ENABLE_1, 0x40);
    if (err == ESP_OK) err = write_reg(REG_INTR_ENABLE_2, 0x00);
    if (err == ESP_OK) err = write_reg(REG_FIFO_WR_PTR, 0x00);
    if (err == ESP_OK) err = write_reg(REG_OVF_COUNTER, 0x00);
    if (err == ESP_OK) err = write_reg(REG_FIFO_RD_PTR, 0x00);
    if (err == ESP_OK) err = write_reg(REG_FIFO_CONFIG, 0x50); /* avg=4, rollover=1 */
    if (err == ESP_OK) err = write_reg(REG_MODE_CONFIG, 0x03); /* SpO2 */
    if (err == ESP_OK) err = write_reg(REG_SPO2_CONFIG, 0x27); /* 4096nA, 100Hz, 411us */
    if (err == ESP_OK) err = write_reg(REG_LED1_PA, 0x24);
    if (err == ESP_OK) err = write_reg(REG_LED2_PA, 0x24);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MAX30102 configuration failed: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG,
             "MAX30102 ready, part=0x%02X revision=0x%02X, SDA=%d SCL=%d INT=%d",
             part_id,
             revision,
             sda_gpio,
             scl_gpio,
             int_gpio);
    return ESP_OK;
}

esp_err_t max30102_read_sample(max30102_sample_t *sample)
{
    if (!s_initialized || sample == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t pointers[3] = {0};
    esp_err_t err = read_regs(REG_FIFO_WR_PTR, pointers, sizeof(pointers));
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t write_ptr = pointers[0] & 0x1F;
    const uint8_t read_ptr = pointers[2] & 0x1F;
    if (write_ptr == read_ptr) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t raw[6] = {0};
    err = read_regs(REG_FIFO_DATA, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    sample->red = ((uint32_t)raw[0] << 16 | (uint32_t)raw[1] << 8 | raw[2]) & 0x3FFFF;
    sample->ir = ((uint32_t)raw[3] << 16 | (uint32_t)raw[4] << 8 | raw[5]) & 0x3FFFF;
    return ESP_OK;
}

esp_err_t max30102_shutdown(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }
    esp_err_t err = write_reg(REG_MODE_CONFIG, 0x80);
    s_initialized = false;
    return err;
}
