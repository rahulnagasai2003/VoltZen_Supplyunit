#pragma once
/*
 * nvs_manager.h — Supply Unit
 *
 * Unified NVS read/write for all persistent configuration.
 * Namespace: "supply_cfg"
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─── WiFi Credentials ────────────────────────────────────────────────────────
/**
 * @brief Save WiFi SSID and password to NVS.
 */
esp_err_t nvs_save_wifi(const char *ssid, const char *password);

/**
 * @brief Load WiFi credentials from NVS.
 * @return true if credentials were found, false otherwise.
 */
bool nvs_load_wifi(char *ssid, size_t ssid_len, char *password, size_t pass_len);

// ─── Thermocouple Type ────────────────────────────────────────────────────────
/**
 * @brief Save thermocouple type string ("J" or "K") to NVS.
 */
esp_err_t nvs_save_tc_type(const char *tc_type);

/**
 * @brief Load thermocouple type from NVS.
 * @return true if found, false if not set (caller should default to "K").
 */
bool nvs_load_tc_type(char *tc_type, size_t len);

// ─── PF Thresholds ────────────────────────────────────────────────────────────
typedef struct {
    float pf_high;
    float pf_low;
    float temp_min;
    float temp_max;
    float fan_limit;
} supply_thresholds_t;

/**
 * @brief Save power-factor and temperature thresholds to NVS.
 */
esp_err_t nvs_save_thresholds(const supply_thresholds_t *thresh);

/**
 * @brief Load thresholds from NVS.
 * @return true if found, false if not yet set.
 */
bool nvs_load_thresholds(supply_thresholds_t *thresh);

// ─── Oven MAC Address ─────────────────────────────────────────────────────────
/**
 * @brief Save the oven ESP32 MAC address (6 bytes) to NVS.
 */
esp_err_t nvs_save_oven_mac(const uint8_t mac[6]);

/**
 * @brief Load the oven ESP32 MAC address from NVS.
 * @return true if a MAC was previously stored.
 */
bool nvs_load_oven_mac(uint8_t mac[6]);

#ifdef __cplusplus
}
#endif
