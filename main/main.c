/*
 * main.c — Supply Unit ESP32
 *
 * Startup sequence:
 *   1. NVS init
 *   2. BLE provisioning (skipped if already provisioned)
 *   3. WiFi STA connect
 *   4. AWS IoT MQTT init
 *   5. ESP-NOW master init
 *   6. FreeRTOS tasks: PZEM polling + 5-minute telemetry
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "ble_provisioning.h"
#include "nvs_manager.h"
#include "wifi_manager.h"
#include "aws_iot.h"
#include "pzem004t.h"
#include "espnow_master.h"

static const char *TAG = "MAIN";

// ─── Intervals ────────────────────────────────────────────────────────────────
#define TELEMETRY_INTERVAL_MS   (5 * 60 * 1000)   // 5 minutes
#define PZEM_POLL_INTERVAL_MS   1000               // 1 second

// ─── Shared PZEM Data ─────────────────────────────────────────────────────────
static pzem_3phase_t       s_supply_data  = {0};
static SemaphoreHandle_t   s_supply_mux  = NULL;
static TaskHandle_t        s_telemetry_task_handle = NULL;

// ─── Trigger telemetry manually ──────────────────────────────────────────────
void trigger_telemetry_publish(void)
{
    if (s_telemetry_task_handle) {
        xTaskNotifyGive(s_telemetry_task_handle);
    }
}

// ─── Thresholds (loaded from NVS, updated by MQTT callback) ──────────────────
static aws_thresholds_t    s_thresholds  = {
    .pf_high        = 0.99f,   // no-op defaults until app sets them
    .pf_low         = 0.0f,
    .temp_min       = 0.0f,
    .temp_max       = 9999.0f
};

// ─── AWS MQTT Callbacks ───────────────────────────────────────────────────────
static void on_aws_connected(void)
{
    ESP_LOGI(TAG, "AWS IoT connected");

    // Publish Telemetry upon connection
    pzem_3phase_t supply;
    if (s_supply_mux) {
        xSemaphoreTake(s_supply_mux, portMAX_DELAY);
        supply = s_supply_data;
        xSemaphoreGive(s_supply_mux);
    } else {
        memset(&supply, 0, sizeof(supply));
    }

    oven_espnow_data_t oven = {0};
    bool oven_fresh = espnow_get_oven_data(&oven);
    int8_t rssi = wifi_manager_get_rssi();

    if (oven_fresh) {
        aws_iot_publish_telemetry(&supply, &oven, rssi, s_thresholds.pf_low);
    } else {
        aws_iot_publish_telemetry(&supply, NULL, rssi, s_thresholds.pf_low);
    }
    ESP_LOGI(TAG, "Gateway Telemetry published on connect.");
}

static void on_refresh_request(void)
{
    ESP_LOGI(TAG, "Refresh request received via MQTT");
    
    // Publish Telemetry upon request
    pzem_3phase_t supply;
    if (s_supply_mux) {
        xSemaphoreTake(s_supply_mux, portMAX_DELAY);
        supply = s_supply_data;
        xSemaphoreGive(s_supply_mux);
    } else {
        memset(&supply, 0, sizeof(supply));
    }

    oven_espnow_data_t oven = {0};
    bool oven_fresh = espnow_get_oven_data(&oven);
    int8_t rssi = wifi_manager_get_rssi();

    if (oven_fresh) {
        aws_iot_publish_telemetry(&supply, &oven, rssi, s_thresholds.pf_low);
    } else {
        aws_iot_publish_telemetry(&supply, NULL, rssi, s_thresholds.pf_low);
    }
    ESP_LOGI(TAG, "Gateway Telemetry published on request.");
}

static void on_threshold_received(const aws_thresholds_t *thresh)
{
    s_thresholds.pf_high = thresh->pf_high;
    s_thresholds.pf_low = thresh->pf_low;
    s_thresholds.temp_min = thresh->temp_min;
    s_thresholds.temp_max = thresh->temp_max;
    s_thresholds.fan_limit = thresh->fan_limit;

    ESP_LOGI(TAG, "Thresholds updated: pf_high=%.2f pf_low=%.2f min=%.1f max=%.1f fan_limit=%.2f",
             thresh->pf_high, thresh->pf_low, thresh->temp_min, thresh->temp_max, thresh->fan_limit);

    // Save to NVS
    supply_thresholds_t nvs_thresh = {
        .pf_high        = thresh->pf_high,
        .pf_low         = thresh->pf_low,
        .temp_min       = thresh->temp_min,
        .temp_max       = thresh->temp_max,
        .fan_limit      = thresh->fan_limit
    };
    nvs_save_thresholds(&nvs_thresh);

    // Forward temperature threshold to oven via ESP-NOW
    if (thresh->tc_type[0] != '\0') {
        nvs_save_tc_type(thresh->tc_type);
    }
    espnow_send_threshold_update(thresh->temp_min, thresh->temp_max, thresh->tc_type);
}

// ─── PZEM Poll Task (1 second) ────────────────────────────────────────────────
static void pzem_poll_task(void *arg)
{
    ESP_LOGI(TAG, "PZEM poll task started");
    while (1) {
        pzem_3phase_t new_data;
        if (pzem_read_all(&new_data) == ESP_OK) {
            xSemaphoreTake(s_supply_mux, portMAX_DELAY);
            s_supply_data = new_data;
            xSemaphoreGive(s_supply_mux);

            // Check PF threshold breach (only if power > 100W to avoid false alarms)
            if (new_data.total_power > 100.0f) {
                if (new_data.avg_pf < s_thresholds.pf_low && s_thresholds.pf_low > 0.0f) {
                    ESP_LOGW(TAG, "LOW_PF alert: pf=%.2f threshold=%.2f",
                             new_data.avg_pf, s_thresholds.pf_low);
                    trigger_telemetry_publish();
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(PZEM_POLL_INTERVAL_MS));
    }
}

// ─── Telemetry Task ───────────────────────────────────────────────────────────
static void telemetry_task(void *arg)
{
    ESP_LOGI(TAG, "Telemetry task started — waiting for notifications");

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (!wifi_manager_is_connected()) {
            ESP_LOGW(TAG, "WiFi not connected — skipping telemetry publish");
            continue;
        }

        // Get latest supply data
        pzem_3phase_t supply;
        xSemaphoreTake(s_supply_mux, portMAX_DELAY);
        supply = s_supply_data;
        xSemaphoreGive(s_supply_mux);

        // Get latest oven data
        oven_espnow_data_t oven = {0};
        bool oven_fresh = espnow_get_oven_data(&oven);
        int8_t rssi = wifi_manager_get_rssi();

        if (oven_fresh) {
            aws_iot_publish_telemetry(&supply, &oven, rssi, s_thresholds.pf_low);
        } else {
            aws_iot_publish_telemetry(&supply, NULL, rssi, s_thresholds.pf_low);
        }

        ESP_LOGI(TAG, "Telemetry published.");
    }
}

// ─── Entry Point ──────────────────────────────────────────────────────────────
void app_main(void)
{
    ESP_LOGI(TAG, "=== Supply Unit Starting ===");

    // 1. NVS init
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Load persisted thresholds if available
    supply_thresholds_t nvs_thresh = {0};
    if (nvs_load_thresholds(&nvs_thresh)) {
        s_thresholds.pf_high        = nvs_thresh.pf_high;
        s_thresholds.pf_low         = nvs_thresh.pf_low;
        s_thresholds.temp_min       = nvs_thresh.temp_min;
        s_thresholds.temp_max       = nvs_thresh.temp_max;
    }

    s_supply_mux = xSemaphoreCreateMutex();

    // 2. BLE provisioning (blocks until done or skipped)
    char ssid[64]    = {0};
    char password[64] = {0};
    ble_provisioning_start(ssid, password);

    ESP_LOGI(TAG, "Using SSID: %s", ssid);

    // 3. WiFi STA connect
    wifi_manager_init(ssid, password);

    // 4. AWS IoT MQTT
    aws_iot_set_callbacks(on_aws_connected, on_threshold_received, on_refresh_request);
    aws_iot_init();

    // 5. ESP-NOW master (load oven MAC from NVS if previously known)
    espnow_master_init(ssid, password);

    // Initialize ESP-NOW master's threshold and TC type
    char tc_type[4] = "K"; // Default to K if nothing is saved
    nvs_load_tc_type(tc_type, sizeof(tc_type));
    espnow_send_threshold_update(s_thresholds.temp_min, s_thresholds.temp_max, tc_type);

    // 6. PZEM init + poll task
    if (pzem_init() == ESP_OK) {
        xTaskCreate(pzem_poll_task, "pzem_poll", 4096, NULL, 5, NULL);
    } else {
        ESP_LOGE(TAG, "PZEM init failed!");
    }

    // 7. Event-driven telemetry task
    xTaskCreate(telemetry_task, "telemetry", 8192, NULL, 4, &s_telemetry_task_handle);

    ESP_LOGI(TAG, "All tasks started. Supply unit running.");
}
