#include "espnow_master.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_manager.h"
#include <string.h>
#include <time.h>

extern void trigger_telemetry_publish(void);

static const char *TAG = "ESPNOW_MST"; // 0x30, 0x76, 0xF5, 0xBA, 0x25, 0x1E
// ─── State
// ────────────────────────────────────────────────────────────────────
static uint8_t s_oven_mac[6] = {0x30, 0x76, 0xF5,
                                0xBA, 0x25, 0x1C}; // Hardcoded oven MAC
static bool s_oven_registered = false;
static uint8_t s_missed_heartbeats = 0;
static bool s_oven_online = false;

// Shared oven data (written from recv callback, read from main task)
static oven_espnow_data_t s_oven_data = {0};
static SemaphoreHandle_t s_oven_data_mux = NULL;

// Provisioning data to forward in HANDSHAKE_ACK
static char s_wifi_ssid[64] = {0};
static char s_wifi_password[64] = {0};
static char s_tc_type[4] = "J";
static float s_temp_min = 0.0f;
static float s_temp_max = 9999.0f;

#define HEARTBEAT_INTERVAL_MS 60000
#define MAX_MISSED_HEARTBEATS 3

// ─── Helpers
// ──────────────────────────────────────────────────────────────────
static void register_oven_peer(const uint8_t mac[6]) {
  esp_now_peer_info_t peer = {0};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0; // 0 = same channel as current WiFi
  peer.encrypt = false;

  if (!esp_now_is_peer_exist(mac)) {
    esp_now_add_peer(&peer);
    ESP_LOGI(TAG, "Oven peer registered: %02X:%02X:%02X:%02X:%02X:%02X", mac[0],
             mac[1], mac[2], mac[3], mac[4], mac[5]);
  }
}

static void send_handshake_ack(const uint8_t *oven_mac) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "type", ESPNOW_MSG_HANDSHAKE_ACK);
  cJSON_AddStringToObject(root, "ssid", s_wifi_ssid);
  cJSON_AddStringToObject(root, "password", s_wifi_password);
  cJSON_AddStringToObject(root, "tc_type", s_tc_type);
  cJSON_AddNumberToObject(root, "temp_min", s_temp_min);
  cJSON_AddNumberToObject(root, "temp_max", s_temp_max);

  char *payload = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!payload)
    return;

  esp_err_t err =
      esp_now_send(oven_mac, (const uint8_t *)payload, strlen(payload) + 1);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "HANDSHAKE_ACK sent to oven. SSID=%s TC=%s Min=%.1f Max=%.1f",
             s_wifi_ssid, s_tc_type, s_temp_min, s_temp_max);
  } else {
    ESP_LOGE(TAG, "Failed to send HANDSHAKE_ACK, err: %s",
             esp_err_to_name(err));
  }
  free(payload);
}

// ─── Receive Callback
// ─────────────────────────────────────────────────────────
static void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                           const uint8_t *data, int data_len) {
  if (data_len < 2)
    return;

  // Parse JSON payload
  cJSON *root = cJSON_Parse((const char *)data);
  if (!root) {
    ESP_LOGE(TAG, "Failed to parse ESP-NOW JSON: %.*s", data_len, data);
    return;
  }

  cJSON *type_item = cJSON_GetObjectItem(root, "type");
  if (!cJSON_IsNumber(type_item)) {
    cJSON_Delete(root);
    return;
  }
  int msg_type = (int)type_item->valuedouble;

  switch (msg_type) {

  case ESPNOW_MSG_HANDSHAKE_REQ: {
    ESP_LOGI(TAG, "HANDSHAKE_REQ received from oven");
    memcpy(s_oven_mac, recv_info->src_addr, 6);
    s_oven_registered = true;

    // Save oven MAC to NVS so it survives reset
    nvs_save_oven_mac(s_oven_mac);

    register_oven_peer(s_oven_mac);
    send_handshake_ack(s_oven_mac);
    s_oven_online = true;
    s_missed_heartbeats = 0;
    break;
  }

  case ESPNOW_MSG_OVEN_DATA: {
    xSemaphoreTake(s_oven_data_mux, portMAX_DELAY);

    cJSON *id = cJSON_GetObjectItem(root, "oven_id");
    cJSON *temp = cJSON_GetObjectItem(root, "temperature");
    cJSON *f1 = cJSON_GetObjectItem(root, "fan_1_current");
    cJSON *f2 = cJSON_GetObjectItem(root, "fan_2_current");
    cJSON *breach = cJSON_GetObjectItem(root, "threshold_breach");

    if (cJSON_IsString(id))
      strncpy(s_oven_data.oven_id, id->valuestring, 31);
    if (cJSON_IsNumber(temp))
      s_oven_data.temperature = (float)temp->valuedouble;
    if (cJSON_IsNumber(f1))
      s_oven_data.fan_1_current = (float)f1->valuedouble;
    if (cJSON_IsNumber(f2))
      s_oven_data.fan_2_current = (float)f2->valuedouble;
    if (cJSON_IsBool(breach))
      s_oven_data.threshold_breach = cJSON_IsTrue(breach);
    s_oven_data.fresh = true;

    bool should_trigger = false;
    if (s_oven_data.temperature > s_temp_max) {
      should_trigger = true;
    }

    xSemaphoreGive(s_oven_data_mux);

    if (should_trigger) {
      ESP_LOGW(TAG, "High temperature (%.1f > %.1f), triggering telemetry!",
               s_oven_data.temperature, s_temp_max);
      trigger_telemetry_publish();
    }

    ESP_LOGI(TAG, "OVEN_DATA rx: temp=%.1f°C fan1=%.2fA fan2=%.2fA breach=%d",
             s_oven_data.temperature, s_oven_data.fan_1_current,
             s_oven_data.fan_2_current, s_oven_data.threshold_breach);
    break;
  }

  case ESPNOW_MSG_HEARTBEAT_ACK: {
    ESP_LOGD(TAG, "HEARTBEAT_ACK received");
    s_missed_heartbeats = 0;
    s_oven_online = true;
    break;
  }

  default:
    ESP_LOGW(TAG, "Unknown message type: %d", msg_type);
    break;
  }

  cJSON_Delete(root);
}

// ─── Heartbeat Task
// ───────────────────────────────────────────────────────────
static void heartbeat_task(void *arg) {
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS));

    if (!s_oven_registered)
      continue;

    // Build heartbeat request
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "type", ESPNOW_MSG_HEARTBEAT_REQ);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload)
      continue;

    esp_now_send(s_oven_mac, (const uint8_t *)payload, strlen(payload) + 1);
    free(payload);

    s_missed_heartbeats++;
    ESP_LOGD(TAG, "HEARTBEAT_REQ sent (missed=%d)", s_missed_heartbeats);

    if (s_missed_heartbeats >= MAX_MISSED_HEARTBEATS) {
      if (s_oven_online) {
        ESP_LOGW(TAG, "Oven offline — %d heartbeats missed",
                 s_missed_heartbeats);
        s_oven_online = false;
        // The main telemetry task will publish the oven-offline alert
      }
    }
  }
}

// ─── Public API
// ───────────────────────────────────────────────────────────────
esp_err_t espnow_master_init(const char *wifi_ssid, const char *wifi_password) {
  strncpy(s_wifi_ssid, wifi_ssid, sizeof(s_wifi_ssid) - 1);
  strncpy(s_wifi_password, wifi_password, sizeof(s_wifi_password) - 1);

  s_oven_data_mux = xSemaphoreCreateMutex();

  ESP_ERROR_CHECK(esp_now_init());
  esp_now_register_recv_cb(espnow_recv_cb);

  // Register hardcoded oven MAC immediately
  s_oven_registered = true;
  register_oven_peer(s_oven_mac);
  ESP_LOGI(TAG, "Oven MAC hardcoded and registered");

  // Start heartbeat task
  xTaskCreate(heartbeat_task, "espnow_hb", 4096, NULL, 4, NULL);

  ESP_LOGI(TAG, "ESP-NOW master initialized");
  return ESP_OK;
}

bool espnow_get_oven_data(oven_espnow_data_t *out) {
  if (!out)
    return false;
  xSemaphoreTake(s_oven_data_mux, portMAX_DELAY);
  memcpy(out, &s_oven_data, sizeof(oven_espnow_data_t));
  xSemaphoreGive(s_oven_data_mux);
  return out->fresh;
}

esp_err_t espnow_send_threshold_update(float temp_min, float temp_max,
                                       const char *tc_type) {
  s_temp_min = temp_min;
  s_temp_max = temp_max;
  if (tc_type && tc_type[0] != '\0') {
    strncpy(s_tc_type, tc_type, sizeof(s_tc_type) - 1);
  }

  if (!s_oven_registered) {
    ESP_LOGW(TAG, "Oven not yet registered — threshold stored, will send on "
                  "next handshake");
    return ESP_OK;
  }

  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "type", ESPNOW_MSG_THRESHOLD_UPDATE);
  cJSON_AddNumberToObject(root, "temp_min", temp_min);
  cJSON_AddNumberToObject(root, "temp_max", temp_max);
  cJSON_AddStringToObject(root, "tc_type", s_tc_type);
  char *payload = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!payload)
    return ESP_ERR_NO_MEM;

  esp_err_t ret =
      esp_now_send(s_oven_mac, (const uint8_t *)payload, strlen(payload) + 1);
  free(payload);
  ESP_LOGI(TAG, "THRESHOLD_UPDATE sent: Min=%.1f°C Max=%.1f°C, TC: %s",
           temp_min, temp_max, s_tc_type);
  return ret;
}

bool espnow_is_oven_online(void) { return s_oven_online; }
