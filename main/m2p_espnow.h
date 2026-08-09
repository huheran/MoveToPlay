#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "imu_lsm6dsv.h"

#ifdef __cplusplus
extern "C" {
#endif

#define M2P_ESPNOW_CHANNEL             6
#define M2P_ESPNOW_PACKET_VERSION      3
#define M2P_ESPNOW_PACKET_MIN_VERSION  1
#define M2P_ESPNOW_TRACKER_PACKET_VERSION 2
#define M2P_ESPNOW_MAGIC               0x4E50324DU /* "M2PN" in little-endian memory */

typedef enum {
    M2P_ESPNOW_ROLE_DONGLE = 0,
    M2P_ESPNOW_ROLE_TRACKER = 1,
    M2P_ESPNOW_ROLE_BLADE = 2,
} m2p_espnow_role_t;

typedef enum {
    M2P_ESPNOW_PACKET_TRACKER_IMU = 1,
    M2P_ESPNOW_PACKET_BLADE_STATE = 2,
    M2P_ESPNOW_PACKET_BLADE_CONTROL = 3,
} m2p_espnow_packet_type_t;

typedef enum {
    M2P_HEART_RATE_STATE_OFF = 0,
    M2P_HEART_RATE_STATE_WAITING_FOR_FINGER = 1,
    M2P_HEART_RATE_STATE_MEASURING = 2,
    M2P_HEART_RATE_STATE_COMPLETE = 3,
    M2P_HEART_RATE_STATE_FAILED = 4,
} m2p_heart_rate_state_t;

#define M2P_ESPNOW_BLADE_FLAG_PRESSED 0x01U
#define M2P_ESPNOW_TRACKER_FLAG_BATTERY_VALID 0x01U
#define M2P_ESPNOW_BLADE_FLAG_BATTERY_VALID 0x02U
#define M2P_ESPNOW_BLADE_FLAG_HEART_RATE_VALID 0x04U
#define M2P_ESPNOW_BLADE_FLAG_FINGER_PRESENT 0x08U
#define M2P_ESPNOW_BLADE_CONTROL_FLAG_START 0x01U
#define M2P_ESPNOW_BATTERY_PERCENT_UNKNOWN 0xFFU

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t node_id;
    uint8_t flags;
    uint32_t sequence;
    uint32_t timestamp_us;
    float accel_g[3];
    float gyro_dps[3];
    uint16_t battery_mv;
    uint8_t battery_percent;
    uint8_t reserved;
    uint16_t heart_rate_bpm_x10;
    uint8_t heart_rate_state;
    uint8_t heart_rate_remaining_seconds;
} m2p_espnow_packet_t;

typedef m2p_espnow_packet_t m2p_espnow_tracker_packet_t;

typedef struct {
    uint8_t src_addr[6];
    m2p_espnow_packet_t packet;
} m2p_espnow_rx_packet_t;

esp_err_t m2p_espnow_init(m2p_espnow_role_t role, uint8_t channel);
esp_err_t m2p_espnow_send_tracker_sample(uint8_t node_id,
                                          uint32_t sequence,
                                          const imu_sample_t *sample,
                                          bool battery_valid,
                                          uint8_t battery_percent,
                                          uint16_t battery_mv);
esp_err_t m2p_espnow_send_blade_state(uint8_t node_id,
                                       uint32_t sequence,
                                       bool pressed,
                                       uint32_t button_edge_timestamp_us,
                                       bool battery_valid,
                                       uint8_t battery_percent,
                                       uint16_t battery_mv,
                                       bool heart_rate_valid,
                                       uint16_t heart_rate_bpm_x10,
                                       bool finger_present,
                                       m2p_heart_rate_state_t heart_rate_state,
                                       uint8_t heart_rate_remaining_seconds);
esp_err_t m2p_espnow_send_blade_heart_rate_control(uint32_t sequence,
                                                    bool start,
                                                    uint8_t duration_seconds);
bool m2p_espnow_receive(m2p_espnow_rx_packet_t *out_packet, uint32_t timeout_ms);
esp_err_t m2p_espnow_enable_softap(const char *ssid,
                                    const char *password,
                                    uint8_t channel,
                                    uint8_t max_connection);

#ifdef __cplusplus
}
#endif
