#pragma once
/*
 * aws_iot.h — Supply Unit
 *
 * AWS IoT Core MQTT client for the supply unit.
 * Publishes the full combined JSON (3-phase supply + oven status) every 5 minutes.
 * Subscribes to threshold and params/request topics.
 */

#include "esp_err.h"
#include "pzem004t.h"
#include "espnow_master.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─── Threshold Struct ─────────────────────────────────────────────────────────
typedef struct {
    float pf_high;
    float pf_low;
    float temp_min;
    float temp_max;
    float fan_limit;
    char  tc_type[4];
} aws_thresholds_t;

// ─── Callbacks ────────────────────────────────────────────────────────────────
typedef void (*aws_threshold_cb_t)(const aws_thresholds_t *thresholds);
typedef void (*aws_connected_cb_t)(void);
typedef void (*aws_refresh_req_cb_t)(void);

/**
 * @brief Set event callbacks before calling aws_iot_init().
 */
void aws_iot_set_callbacks(aws_connected_cb_t conn_cb,
                           aws_threshold_cb_t thresh_cb,
                           aws_refresh_req_cb_t refresh_cb);

/**
 * @brief Initialize and start the MQTT client (connects to AWS IoT Core).
 */
esp_err_t aws_iot_init(void);

/**
 * @brief Publish the full combined telemetry JSON to AWS.
 *
 * @param supply           3-phase PZEM data
 * @param oven             Latest oven data received via ESP-NOW
 * @param rssi             WiFi signal strength in dBm
 * @param min_pf_threshold The current minimum power factor threshold
 */
esp_err_t aws_iot_publish_telemetry(const pzem_3phase_t      *supply,
                                    const oven_espnow_data_t *oven,
                                    int8_t                    rssi,
                                    float                     min_pf_threshold);

/**
 * @brief Publish ACK to AWS IoT Core (includes full payload like telemetry)
 * Topic: voltzen/gateway/ack/<MAC>
 */
esp_err_t aws_iot_publish_ack(const pzem_3phase_t *supply, 
                              const oven_espnow_data_t *oven,
                              int8_t rssi,
                              float min_pf_threshold);

#ifdef __cplusplus
}
#endif
