#include "nvs_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG      = "NVS_MGR";
static const char *NVS_NS   = "supply_cfg";

// ─── Keys ─────────────────────────────────────────────────────────────────────
#define KEY_SSID        "ssid"
#define KEY_PASS        "password"
#define KEY_TC_TYPE     "tc_type"
#define KEY_THRESHOLDS  "thresholds"
#define KEY_OVEN_MAC    "oven_mac"

// ─── WiFi ─────────────────────────────────────────────────────────────────────
esp_err_t nvs_save_wifi(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_str(h, KEY_SSID, ssid);
    nvs_set_str(h, KEY_PASS, password);
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "WiFi credentials saved (SSID: %s)", ssid);
    return err;
}

bool nvs_load_wifi(char *ssid, size_t ssid_len, char *password, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    bool ok = (nvs_get_str(h, KEY_SSID, ssid, &ssid_len) == ESP_OK) &&
              (nvs_get_str(h, KEY_PASS, password, &pass_len) == ESP_OK);
    nvs_close(h);
    if (ok) ESP_LOGI(TAG, "WiFi loaded from NVS: SSID=%s", ssid);
    return ok;
}

// ─── Thermocouple Type ────────────────────────────────────────────────────────
esp_err_t nvs_save_tc_type(const char *tc_type)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_str(h, KEY_TC_TYPE, tc_type);
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "TC type saved: %s", tc_type);
    return err;
}

bool nvs_load_tc_type(char *tc_type, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = (nvs_get_str(h, KEY_TC_TYPE, tc_type, &len) == ESP_OK);
    nvs_close(h);
    return ok;
}

// ─── Thresholds ───────────────────────────────────────────────────────────────
esp_err_t nvs_save_thresholds(const supply_thresholds_t *thresh)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_blob(h, KEY_THRESHOLDS, thresh, sizeof(supply_thresholds_t));
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Thresholds saved: pf_H=%.2f pf_L=%.2f temp_min=%.1f temp_max=%.1f fan_limit=%.2f",
             thresh->pf_high, thresh->pf_low, thresh->temp_min, thresh->temp_max, thresh->fan_limit);
    return err;
}

bool nvs_load_thresholds(supply_thresholds_t *thresh)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    size_t sz = sizeof(supply_thresholds_t);
    bool ok = (nvs_get_blob(h, KEY_THRESHOLDS, thresh, &sz) == ESP_OK);
    nvs_close(h);
    if (ok) ESP_LOGI(TAG, "Thresholds loaded: pf_H=%.2f pf_L=%.2f temp_min=%.1f temp_max=%.1f fan_limit=%.2f",
                     thresh->pf_high, thresh->pf_low, thresh->temp_min, thresh->temp_max, thresh->fan_limit);
    return ok;
}

// ─── Oven MAC ─────────────────────────────────────────────────────────────────
esp_err_t nvs_save_oven_mac(const uint8_t mac[6])
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_blob(h, KEY_OVEN_MAC, mac, 6);
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Oven MAC saved: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return err;
}

bool nvs_load_oven_mac(uint8_t mac[6])
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    size_t sz = 6;
    bool ok = (nvs_get_blob(h, KEY_OVEN_MAC, mac, &sz) == ESP_OK);
    nvs_close(h);
    if (ok) ESP_LOGI(TAG, "Oven MAC loaded: %02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return ok;
}
