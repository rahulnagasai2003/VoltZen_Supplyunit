#pragma once
/*
 * wifi_manager.h — Supply Unit
 *
 * Manages WiFi STA connection with auto-reconnect.
 * Credentials are loaded from NVS by nvs_manager.
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize WiFi in STA mode and connect to the given SSID.
 *        If ssid is NULL, tries to load credentials from NVS first.
 *        Blocks until connected or max retries exceeded.
 */
esp_err_t wifi_manager_init(const char *ssid, const char *password);

/**
 * @brief Returns true if WiFi is currently connected (has IP).
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Get the current WiFi signal strength in dBm.
 *        Returns 0 if not connected.
 */
int8_t wifi_manager_get_rssi(void);

#ifdef __cplusplus
}
#endif
