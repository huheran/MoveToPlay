#include "m2p_espnow.h"

#include <stddef.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs_flash.h"

static const char *TAG = "m2p_espnow";

#define M2P_ESPNOW_RX_QUEUE_LEN 16
#define M2P_ESPNOW_TX_POWER_QDBM 40 /* 10 dBm, unit is 0.25 dBm. */
#define M2P_ESPNOW_PACKET_V1_SIZE 40

_Static_assert(sizeof(m2p_espnow_packet_t) <= ESP_NOW_MAX_DATA_LEN,
               "MoveToPlay packet must fit in one ESP-NOW v1 packet");
_Static_assert(offsetof(m2p_espnow_packet_t, battery_mv) == M2P_ESPNOW_PACKET_V1_SIZE,
               "MoveToPlay v1 packet size must stay compatible");

static const uint8_t s_broadcast_addr[ESP_NOW_ETH_ALEN] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static QueueHandle_t s_rx_queue = NULL;
static bool s_espnow_ready = false;
static esp_netif_t *s_softap_netif = NULL;
static bool s_softap_ready = false;

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase nvs failed");
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t init_wifi(uint8_t channel)
{
    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "nvs init failed");

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "esp_netif init failed");
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "event loop init failed");
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "wifi ps disable failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_max_tx_power(M2P_ESPNOW_TX_POWER_QDBM), TAG, "wifi tx power failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE), TAG, "wifi channel failed");

    return ESP_OK;
}

static const char *role_name(m2p_espnow_role_t role)
{
    switch (role) {
    case M2P_ESPNOW_ROLE_DONGLE:
        return "dongle";
    case M2P_ESPNOW_ROLE_TRACKER:
        return "tracker";
    case M2P_ESPNOW_ROLE_BLADE:
        return "blade";
    default:
        return "unknown";
    }
}

static bool is_valid_packet(const m2p_espnow_packet_t *packet, int data_len)
{
    if (packet->magic != M2P_ESPNOW_MAGIC ||
        packet->version < M2P_ESPNOW_PACKET_MIN_VERSION ||
        packet->version > M2P_ESPNOW_PACKET_VERSION ||
        data_len < M2P_ESPNOW_PACKET_V1_SIZE ||
        data_len > (int)sizeof(m2p_espnow_packet_t)) {
        return false;
    }

    return packet->type == M2P_ESPNOW_PACKET_TRACKER_IMU ||
           packet->type == M2P_ESPNOW_PACKET_BLADE_STATE;
}

static void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int data_len)
{
    if (s_rx_queue == NULL || recv_info == NULL || recv_info->src_addr == NULL || data == NULL) {
        return;
    }

    if (data_len < M2P_ESPNOW_PACKET_V1_SIZE ||
        data_len > (int)sizeof(m2p_espnow_packet_t)) {
        return;
    }

    m2p_espnow_rx_packet_t rx_packet = {0};
    memcpy(rx_packet.src_addr, recv_info->src_addr, sizeof(rx_packet.src_addr));
    memcpy(&rx_packet.packet, data, (size_t)data_len);

    if (!is_valid_packet(&rx_packet.packet, data_len)) {
        return;
    }

    (void)xQueueSend(s_rx_queue, &rx_packet, 0);
}

static void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info;
    (void)status;
}

static esp_err_t add_broadcast_peer(uint8_t channel)
{
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, s_broadcast_addr, sizeof(peer.peer_addr));
    peer.channel = channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    esp_err_t err = esp_now_add_peer(&peer);
    if (err == ESP_ERR_ESPNOW_EXIST) {
        return ESP_OK;
    }
    return err;
}

esp_err_t m2p_espnow_init(m2p_espnow_role_t role, uint8_t channel)
{
    ESP_RETURN_ON_FALSE(channel >= 1 && channel <= 14, ESP_ERR_INVALID_ARG, TAG, "invalid channel");

    if (s_espnow_ready) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(init_wifi(channel), TAG, "wifi init failed");
    s_rx_queue = xQueueCreate(M2P_ESPNOW_RX_QUEUE_LEN, sizeof(m2p_espnow_rx_packet_t));
    ESP_RETURN_ON_FALSE(s_rx_queue != NULL, ESP_ERR_NO_MEM, TAG, "rx queue alloc failed");

    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "esp-now init failed");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(espnow_recv_cb), TAG, "register recv cb failed");
    ESP_RETURN_ON_ERROR(esp_now_register_send_cb(espnow_send_cb), TAG, "register send cb failed");
    ESP_RETURN_ON_ERROR(add_broadcast_peer(channel), TAG, "add broadcast peer failed");

    uint8_t sta_mac[6] = {0};
    (void)esp_wifi_get_mac(WIFI_IF_STA, sta_mac);

    ESP_LOGI(TAG,
             "ESP-NOW init ok, role=%s, channel=%u, sta_mac=%02X:%02X:%02X:%02X:%02X:%02X, packet_size=%u",
             role_name(role),
             channel,
             sta_mac[0],
             sta_mac[1],
             sta_mac[2],
             sta_mac[3],
             sta_mac[4],
             sta_mac[5],
             (unsigned)sizeof(m2p_espnow_packet_t));

    s_espnow_ready = true;
    return ESP_OK;
}

esp_err_t m2p_espnow_send_tracker_sample(uint8_t node_id,
                                          uint32_t sequence,
                                          const imu_sample_t *sample,
                                          bool battery_valid,
                                          uint8_t battery_percent,
                                          uint16_t battery_mv)
{
    ESP_RETURN_ON_FALSE(s_espnow_ready, ESP_ERR_INVALID_STATE, TAG, "esp-now not ready");
    ESP_RETURN_ON_FALSE(sample != NULL, ESP_ERR_INVALID_ARG, TAG, "sample is NULL");

    if (battery_percent > 100U) {
        battery_valid = false;
        battery_percent = M2P_ESPNOW_BATTERY_PERCENT_UNKNOWN;
        battery_mv = 0;
    }

    m2p_espnow_packet_t packet = {
        .magic = M2P_ESPNOW_MAGIC,
        .version = M2P_ESPNOW_PACKET_VERSION,
        .type = M2P_ESPNOW_PACKET_TRACKER_IMU,
        .node_id = node_id,
        .flags = battery_valid ? M2P_ESPNOW_TRACKER_FLAG_BATTERY_VALID : 0,
        .sequence = sequence,
        .timestamp_us = (uint32_t)esp_timer_get_time(),
        .accel_g = {
            sample->accel_g[0],
            sample->accel_g[1],
            sample->accel_g[2],
        },
        .gyro_dps = {
            sample->gyro_dps[0],
            sample->gyro_dps[1],
            sample->gyro_dps[2],
        },
        .battery_mv = battery_valid ? battery_mv : 0,
        .battery_percent = battery_valid ? battery_percent : M2P_ESPNOW_BATTERY_PERCENT_UNKNOWN,
        .reserved = 0,
    };

    return esp_now_send(s_broadcast_addr, (const uint8_t *)&packet, sizeof(packet));
}

esp_err_t m2p_espnow_send_blade_state(uint8_t node_id,
                                       uint32_t sequence,
                                       bool pressed,
                                       uint32_t button_edge_timestamp_us,
                                       bool battery_valid,
                                       uint8_t battery_percent,
                                       uint16_t battery_mv)
{
    ESP_RETURN_ON_FALSE(s_espnow_ready, ESP_ERR_INVALID_STATE, TAG, "esp-now not ready");

    if (battery_percent > 100U) {
        battery_valid = false;
        battery_percent = M2P_ESPNOW_BATTERY_PERCENT_UNKNOWN;
        battery_mv = 0;
    }

    uint8_t flags = pressed ? M2P_ESPNOW_BLADE_FLAG_PRESSED : 0;
    if (battery_valid) {
        flags |= M2P_ESPNOW_BLADE_FLAG_BATTERY_VALID;
    }

    m2p_espnow_packet_t packet = {
        .magic = M2P_ESPNOW_MAGIC,
        .version = M2P_ESPNOW_PACKET_VERSION,
        .type = M2P_ESPNOW_PACKET_BLADE_STATE,
        .node_id = node_id,
        .flags = flags,
        .sequence = sequence,
        .timestamp_us = button_edge_timestamp_us,
        .battery_mv = battery_valid ? battery_mv : 0,
        .battery_percent = battery_valid ? battery_percent : M2P_ESPNOW_BATTERY_PERCENT_UNKNOWN,
        .reserved = 0,
    };

    return esp_now_send(s_broadcast_addr, (const uint8_t *)&packet, sizeof(packet));
}

bool m2p_espnow_receive(m2p_espnow_rx_packet_t *out_packet, uint32_t timeout_ms)
{
    if (out_packet == NULL || s_rx_queue == NULL) {
        return false;
    }

    return xQueueReceive(s_rx_queue, out_packet, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

esp_err_t m2p_espnow_enable_softap(const char *ssid,
                                    const char *password,
                                    uint8_t channel,
                                    uint8_t max_connection)
{
    ESP_RETURN_ON_FALSE(s_espnow_ready, ESP_ERR_INVALID_STATE, TAG, "esp-now not ready");
    ESP_RETURN_ON_FALSE(ssid != NULL, ESP_ERR_INVALID_ARG, TAG, "softap ssid is NULL");
    ESP_RETURN_ON_FALSE(channel >= 1 && channel <= 14, ESP_ERR_INVALID_ARG, TAG, "invalid softap channel");
    ESP_RETURN_ON_FALSE(max_connection > 0, ESP_ERR_INVALID_ARG, TAG, "invalid softap max_connection");

    const char *ap_password = (password != NULL) ? password : "";
    const size_t ssid_len = strlen(ssid);
    const size_t pass_len = strlen(ap_password);
    ESP_RETURN_ON_FALSE(ssid_len > 0 && ssid_len < 32, ESP_ERR_INVALID_ARG, TAG, "invalid softap ssid length");
    ESP_RETURN_ON_FALSE(pass_len < 64, ESP_ERR_INVALID_ARG, TAG, "invalid softap password length");
    ESP_RETURN_ON_FALSE(pass_len == 0 || pass_len >= 8, ESP_ERR_INVALID_ARG, TAG, "softap password too short");

    if (s_softap_ready) {
        return ESP_OK;
    }

    if (s_softap_netif == NULL) {
        s_softap_netif = esp_netif_create_default_wifi_ap();
        ESP_RETURN_ON_FALSE(s_softap_netif != NULL, ESP_ERR_NO_MEM, TAG, "softap netif create failed");
    }

    wifi_config_t ap_config = {0};
    memcpy(ap_config.ap.ssid, ssid, ssid_len);
    ap_config.ap.ssid_len = ssid_len;
    memcpy(ap_config.ap.password, ap_password, pass_len);
    ap_config.ap.channel = channel;
    ap_config.ap.max_connection = max_connection;
    ap_config.ap.authmode = (pass_len == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    ap_config.ap.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "wifi APSTA mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG, "softap config failed");

    esp_err_t channel_err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (channel_err != ESP_OK) {
        ESP_LOGW(TAG, "softap channel set warning: %s", esp_err_to_name(channel_err));
    }

    ESP_LOGI(TAG,
             "SoftAP enabled, ssid=%s channel=%u auth=%s max_connection=%u",
             ssid,
             channel,
             (pass_len == 0) ? "open" : "wpa2",
             max_connection);

    s_softap_ready = true;
    return ESP_OK;
}
