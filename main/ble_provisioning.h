#pragma once
/*
 * ble_provisioning.h — Supply Unit
 *
 * BLE-based WiFi provisioning using ESP-IDF wifi_prov_mgr with NimBLE.
 * The app sends SSID, password, and thermocouple type ("J" or "K")
 * via the standard BLE provisioning protocol plus a custom endpoint.
 *
 * On first boot (no credentials in NVS):
 *   - BLE advertises as "SupplyUnit_XXXX" (last 4 hex of MAC)
 *   - App connects and sends SSID + password + tc_type
 *   - Credentials saved to NVS
 *   - BLE is shut down and WiFi STA starts
 *
 * On subsequent boots:
 *   - ble_provisioning_start() returns immediately (skips BLE)
 *   - Caller reads credentials from NVS via nvs_manager
 */

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start BLE provisioning if credentials are not in NVS.
 *
 * Blocks until provisioning is complete (credentials received and saved).
 * If credentials already exist in NVS, returns immediately.
 *
 * @param ssid_out      Buffer to receive the provisioned SSID (size >= 64)
 * @param pass_out      Buffer to receive the provisioned password (size >= 64)
 * @return true  if credentials were received via BLE (fresh provisioning)
 * @return false if credentials were already in NVS (skipped BLE)
 */
bool ble_provisioning_start(char *ssid_out, char *pass_out);

#ifdef __cplusplus
}
#endif
