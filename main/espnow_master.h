#pragma once
/*
 * espnow_master.h — Supply Unit
 *
 * Handles ESP-NOW communication toward the Oven Unit:
 *  - Receives HANDSHAKE_REQ, saves oven MAC, replies with WiFi creds + TC type + threshold
 *  - Sends periodic HEARTBEAT_REQ every 60 s
 *  - Detects oven offline if 3 consecutive heartbeats are missed
 *  - Receives OVEN_DATA and stores it in a shared global struct
 *  - Forwards THRESHOLD_UPDATE to the oven when app changes temp threshold
 */

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─── Message Type Codes ───────────────────────────────────────────────────────
#define ESPNOW_MSG_HANDSHAKE_REQ    0x01
#define ESPNOW_MSG_HANDSHAKE_ACK    0x02
#define ESPNOW_MSG_OVEN_DATA        0x03
#define ESPNOW_MSG_THRESHOLD_UPDATE 0x04
#define ESPNOW_MSG_HEARTBEAT_REQ    0x05
#define ESPNOW_MSG_HEARTBEAT_ACK    0x06

// ─── Oven Data Structure (received from oven via ESP-NOW) ────────────────────
typedef struct {
    char  oven_id[32];
    float temperature;
    float fan_1_current;
    float fan_2_current;
    bool  threshold_breach;
    bool  fresh;              // false = no data received yet
} oven_espnow_data_t;

/**
 * @brief Initialize ESP-NOW master.
 *        Must be called after WiFi is up (same channel used by ESP-NOW).
 *
 * @param wifi_ssid       SSID to forward to oven in HANDSHAKE_ACK
 * @param wifi_password   Password to forward to oven
 * @param tc_type         Thermocouple type string ("J" or "K")
 */
esp_err_t espnow_master_init(const char *wifi_ssid,
                              const char *wifi_password);

/**
 * @brief Copy the latest oven data into *out.
 *        Returns false if no OVEN_DATA has been received yet.
 */
bool espnow_get_oven_data(oven_espnow_data_t *out);

/**
 * @brief Send a THRESHOLD_UPDATE message to the oven.
 *        Called when a new temp threshold arrives from AWS MQTT.
 */
esp_err_t espnow_send_threshold_update(float temp_min, float temp_max, const char *tc_type);

/**
 * @brief Returns true if at least one HEARTBEAT_ACK has been received
 *        within the last 3 heartbeat cycles (180 s).
 */
bool espnow_is_oven_online(void);

#ifdef __cplusplus
}
#endif
