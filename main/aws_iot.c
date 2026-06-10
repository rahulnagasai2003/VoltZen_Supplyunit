#include "aws_iot.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include "esp_mac.h"
#else
#include "esp_system.h"
#endif

#include "esp_sntp.h"
#include <sys/time.h>

static const char *TAG = "AWS_IOT";

// ─── AWS IoT Core Settings
// endpoint as the existing templyzen project
static const char *HOSTNAME = "akpcatpshas5b-ats.iot.ap-south-1.amazonaws.com";
static const int PORT = 8883;
static const char *CLIENT_ID = "voltzen_master_gateway";

// ─── Embedded Certificates (via CMakeLists EMBED_TXTFILES) ───────────────────
extern const uint8_t
    client_cert_pem_start[] asm("_binary_client_cert_pem_start");
extern const uint8_t client_cert_pem_end[] asm("_binary_client_cert_pem_end");
extern const uint8_t client_key_pem_start[] asm("_binary_client_key_pem_start");
extern const uint8_t client_key_pem_end[] asm("_binary_client_key_pem_end");
extern const uint8_t root_cert_pem_start[] asm("_binary_root_cert_pem_start");
extern const uint8_t root_cert_pem_end[] asm("_binary_root_cert_pem_end");

// ─── State
// ────────────────────────────────────────────────────────────────────
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static char s_mac_str[18] = {0};
static aws_connected_cb_t s_conn_cb = NULL;
static aws_threshold_cb_t s_thresh_cb = NULL;
static aws_refresh_req_cb_t s_refresh_cb = NULL;

// ─── Internal Helpers
// ─────────────────────────────────────────────────────────
static esp_err_t publish_json(const char *topic, cJSON *root) {
  if (!s_mqtt_client) {
    cJSON_Delete(root);
    return ESP_FAIL;
  }

  char *payload = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!payload)
    return ESP_ERR_NO_MEM;

  int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, 1, 0);
  ESP_LOGI(TAG, "Published to Topic: %s", topic);
  ESP_LOGI(TAG, "Payload: %s", payload);
  free(payload);
  return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

static cJSON *build_phase_object(const pzem_data_t *p) {
  cJSON *obj = cJSON_CreateObject();
  cJSON_AddNumberToObject(obj, "voltage", p->voltage);
  cJSON_AddNumberToObject(obj, "current", p->current);
  cJSON_AddNumberToObject(obj, "power", p->power);
  cJSON_AddNumberToObject(obj, "energy", p->energy);
  cJSON_AddNumberToObject(obj, "frequency", p->frequency);
  return obj;
}

// ─── MQTT Event Handler
// ───────────────────────────────────────────────────────
static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
  esp_mqtt_event_handle_t event = event_data;

  switch ((esp_mqtt_event_id_t)event_id) {

  case MQTT_EVENT_CONNECTED: {
    ESP_LOGI(TAG, "MQTT connected to AWS IoT Core");

    char topic[128];
    // Subscribe: threshold updates from app
    snprintf(topic, sizeof(topic), "voltzen/unit/%s/thresholds", s_mac_str);
    esp_mqtt_client_subscribe(s_mqtt_client, topic, 1);

    // Subscribe: on-demand params request
    esp_mqtt_client_subscribe(s_mqtt_client, "voltzen/unit/request", 1);

    if (s_conn_cb)
      s_conn_cb();
    break;
  }

  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGW(TAG, "MQTT disconnected — client will auto-reconnect");
    break;

  case MQTT_EVENT_DATA: {
    char topic_buf[128] = {0};
    snprintf(topic_buf, sizeof(topic_buf), "%.*s", event->topic_len,
             event->topic);

    char thresh_topic[128];
    snprintf(thresh_topic, sizeof(thresh_topic), "voltzen/unit/%s/thresholds",
             s_mac_str);

    if (strcmp(topic_buf, thresh_topic) == 0 && s_thresh_cb) {
      cJSON *json = cJSON_ParseWithLength(event->data, event->data_len);
      if (json) {
        aws_thresholds_t t = {0};
        
        // 1. Temp max
        cJSON *t_max = cJSON_GetObjectItem(json, "max_temp_threshold");
        if (!t_max) t_max = cJSON_GetObjectItem(json, "max_temp");
        
        // 2. Temp min
        cJSON *t_min = cJSON_GetObjectItem(json, "min_temp_threshold");
        if (!t_min) t_min = cJSON_GetObjectItem(json, "min_temp");
        
        // 3. PF low (low_pf_range or pf_limit)
        cJSON *t_pf_low  = cJSON_GetObjectItem(json, "low_pf_range");
        if (!t_pf_low) t_pf_low = cJSON_GetObjectItem(json, "pf_limit");

        // 4. PF high
        cJSON *t_pf_high = cJSON_GetObjectItem(json, "high_pf_range");

        // 5. TC Type
        cJSON *t_tc_type = cJSON_GetObjectItem(json, "temperature_type");

        // 6. Fan Limit
        cJSON *t_fan_limit = cJSON_GetObjectItem(json, "fan_limit");
       
        if (cJSON_IsNumber(t_max))
          t.temp_max = (float)t_max->valuedouble;
        if (cJSON_IsNumber(t_min))
          t.temp_min = (float)t_min->valuedouble;
        if (cJSON_IsNumber(t_pf_low))
          t.pf_low = (float)t_pf_low->valuedouble;
        if (cJSON_IsNumber(t_pf_high))
          t.pf_high = (float)t_pf_high->valuedouble;
        if (cJSON_IsNumber(t_fan_limit))
          t.fan_limit = (float)t_fan_limit->valuedouble;
        if (cJSON_IsString(t_tc_type) && t_tc_type->valuestring != NULL) {
            strncpy(t.tc_type, t_tc_type->valuestring, sizeof(t.tc_type) - 1);
        }
       
        s_thresh_cb(&t);
        cJSON_Delete(json);
      }
    } else if (strcmp(topic_buf, "voltzen/unit/request") == 0 && s_refresh_cb) {
      ESP_LOGI(TAG, "Incoming Message on Topic: %s", topic_buf);
      ESP_LOGI(TAG, "Incoming Payload: %.*s", event->data_len, event->data);
      cJSON *json = cJSON_ParseWithLength(event->data, event->data_len);
      if (json) {
        cJSON *cmd = cJSON_GetObjectItem(json, "command");
        if (cJSON_IsString(cmd) &&
            strcmp(cmd->valuestring, "REFRESH_TELEMETRY") == 0) {
          s_refresh_cb();
        }
        cJSON_Delete(json);
      }
    }
    break;
  }

  case MQTT_EVENT_ERROR:
    ESP_LOGE(TAG, "MQTT error");
    break;

  default:
    break;
  }
}

// ─── Public API
// ───────────────────────────────────────────────────────────────
void aws_iot_set_callbacks(aws_connected_cb_t conn_cb,
                           aws_threshold_cb_t thresh_cb,
                           aws_refresh_req_cb_t refresh_cb) {
  s_conn_cb = conn_cb;
  s_thresh_cb = thresh_cb;
  s_refresh_cb = refresh_cb;
}

static void aws_start_task(void *arg) {
  int retry = 0;
  // Increased retry to 30 (60 seconds) to handle slow DNS/NTP on hotspots
  while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < 30) {
    ESP_LOGI(TAG, "Waiting for system time to be set... (%d/30)", retry);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
  
  time_t now;
  time(&now);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  ESP_LOGI(TAG, "Current time: %s", asctime(&timeinfo));

  esp_mqtt_client_start(s_mqtt_client);
  vTaskDelete(NULL);
}

esp_err_t aws_iot_init(void) {
  ESP_LOGI(TAG, "Initializing SNTP for AWS TLS validation...");
  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();

  uint8_t mac[6];
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  snprintf(s_mac_str, sizeof(s_mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  ESP_LOGI(TAG, "Supply Unit MAC: %s", s_mac_str);

  char lwt_topic[128];
  snprintf(lwt_topic, sizeof(lwt_topic), "voltzen/unit/%s/offline", s_mac_str);
  char lwt_msg[64];
  snprintf(lwt_msg, sizeof(lwt_msg), "{\"online\":false,\"device_id\":\"%s\"}",
           s_mac_str);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  esp_mqtt_client_config_t cfg = {
      .broker.address.hostname = HOSTNAME,
      .broker.address.transport = MQTT_TRANSPORT_OVER_SSL,
      .broker.address.port = PORT,
      .broker.verification.certificate = (const char *)root_cert_pem_start,
      .credentials.client_id = CLIENT_ID,
      .credentials.authentication.certificate =
          (const char *)client_cert_pem_start,
      .credentials.authentication.key = (const char *)client_key_pem_start,
  };
#else
  esp_mqtt_client_config_t cfg = {
      .host = HOSTNAME,
      .transport = MQTT_TRANSPORT_OVER_SSL,
      .port = PORT,
      .client_id = CLIENT_ID,
      .cert_pem = (const char *)root_cert_pem_start,
      .client_cert_pem = (const char *)client_cert_pem_start,
      .client_key_pem = (const char *)client_key_pem_start,
  };
#endif

  s_mqtt_client = esp_mqtt_client_init(&cfg);
  if (!s_mqtt_client)
    return ESP_FAIL;

  esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                 mqtt_event_handler, NULL);
  
  // Start the background task to wait for SNTP and then connect MQTT
  xTaskCreate(aws_start_task, "aws_start", 4096, NULL, 5, NULL);
  
  return ESP_OK;
}

esp_err_t aws_iot_publish_telemetry(const pzem_3phase_t *supply,
                                    const oven_espnow_data_t *oven, int8_t rssi,
                                    float min_pf_threshold) {
  time_t now;
  time(&now);

  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "device_id", s_mac_str);
  cJSON_AddNumberToObject(root, "timestamp", (double)now);
  cJSON_AddNumberToObject(root, "pf", supply->avg_pf);

  if (!oven || oven->oven_id[0] == '\0') {
      cJSON_AddNumberToObject(root, "min_pf_threshold", min_pf_threshold);
  }

  cJSON_AddNumberToObject(root, "signal_strength", rssi);
  cJSON_AddStringToObject(root, "firmware_version", "v1.2.0");
  cJSON_AddStringToObject(root, "build_version", "b104");
  cJSON_AddStringToObject(root, "hardware_version", "revC");

  // ── unit_supply ──────────────────────────────────────────────────────────
  cJSON *unit_supply = cJSON_CreateObject();
  cJSON_AddItemToObject(unit_supply, "phase_1",
                        build_phase_object(&supply->phase[0]));
  cJSON_AddItemToObject(unit_supply, "phase_2",
                        build_phase_object(&supply->phase[1]));
  cJSON_AddItemToObject(unit_supply, "phase_3",
                        build_phase_object(&supply->phase[2]));
  cJSON_AddItemToObject(root, "unit_supply", unit_supply);

  char topic[128];
  if (oven && oven->oven_id[0] != '\0') {
    cJSON *oven_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(oven_obj, "oven_id", oven->oven_id);
    cJSON_AddNumberToObject(oven_obj, "temperature", oven->temperature);
    cJSON_AddNumberToObject(oven_obj, "fan_1_current", oven->fan_1_current);
    cJSON_AddNumberToObject(oven_obj, "fan_2_current", oven->fan_2_current);
    cJSON_AddItemToObject(root, "oven_status", oven_obj);
    snprintf(topic, sizeof(topic), "voltzen/unit/%s/%s/telemetry", s_mac_str, oven->oven_id);
  } else {
    snprintf(topic, sizeof(topic), "voltzen/unit/%s/telemetry", s_mac_str);
  }

  return publish_json(topic, root);
}

esp_err_t aws_iot_publish_ack(const pzem_3phase_t *supply,
                              const oven_espnow_data_t *oven, int8_t rssi,
                              float min_pf_threshold) {
  time_t now;
  time(&now);

  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "device_id", s_mac_str);
  cJSON_AddNumberToObject(root, "timestamp", (double)now);
  cJSON_AddNumberToObject(root, "pf", supply->avg_pf);
  cJSON_AddNumberToObject(root, "min_pf_threshold", min_pf_threshold);
  cJSON_AddNumberToObject(root, "signal_strength", rssi);
  cJSON_AddStringToObject(root, "firmware_version", "v1.2.0");
  cJSON_AddStringToObject(root, "build_version", "b104");
  cJSON_AddStringToObject(root, "hardware_version", "revC");

  cJSON *unit_supply = cJSON_CreateObject();
  cJSON_AddItemToObject(unit_supply, "phase_1", build_phase_object(&supply->phase[0]));
  cJSON_AddItemToObject(unit_supply, "phase_2", build_phase_object(&supply->phase[1]));
  cJSON_AddItemToObject(unit_supply, "phase_3", build_phase_object(&supply->phase[2]));
  cJSON_AddItemToObject(root, "unit_supply", unit_supply);

  if (oven && oven->oven_id[0] != '\0') {
    cJSON *oven_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(oven_obj, "oven_id", oven->oven_id);
    cJSON_AddNumberToObject(oven_obj, "temperature", oven->temperature);
    cJSON_AddNumberToObject(oven_obj, "fan_1_current", oven->fan_1_current);
    cJSON_AddNumberToObject(oven_obj, "fan_2_current", oven->fan_2_current);
    cJSON_AddItemToObject(root, "oven_status", oven_obj);
  }

  char topic[128];
  snprintf(topic, sizeof(topic), "voltzen/gateway/ack/%s", s_mac_str);
  return publish_json(topic, root);
}
