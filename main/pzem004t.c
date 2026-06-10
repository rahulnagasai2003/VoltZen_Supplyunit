#include "pzem004t.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "PZEM";

// ─── Phase Config
// ─────────────────────────────────────────────────────────────
static const struct {
  int rx_gpio;
  uint8_t modbus_addr;
  const char *name;
} phase_cfg[PZEM_PHASE_COUNT] = {
    [PZEM_PHASE_1] = {PZEM_RX_PHASE_1, PZEM_ADDR_PHASE_1, "Phase-1"},
    [PZEM_PHASE_2] = {PZEM_RX_PHASE_2, PZEM_ADDR_PHASE_2, "Phase-2"},
    [PZEM_PHASE_3] = {PZEM_RX_PHASE_3, PZEM_ADDR_PHASE_3, "Phase-3"},
};

// ─── Modbus RTU CRC-16
// ────────────────────────────────────────────────────────
static uint16_t crc16(const uint8_t *data, uint16_t len) {
  uint16_t crc = 0xFFFF;
  for (uint16_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t j = 0; j < 8; j++) {
      crc = (crc & 0x0001) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
  }
  return crc;
}

// ─── Init
// ─────────────────────────────────────────────────────────────────────
esp_err_t pzem_init(void) {
  uart_config_t cfg = {
      .baud_rate = PZEM_BAUD_RATE,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
  };

  esp_err_t ret = uart_driver_install(PZEM_UART_PORT, 512, 0, 0, NULL, 0);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(ret));
    return ret;
  }
  ret = uart_param_config(PZEM_UART_PORT, &cfg);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "UART param config failed: %s", esp_err_to_name(ret));
    return ret;
  }
  // Enable pull-ups on all RX pins immediately so disconnected pins don't float
  // and cause UART noise
  gpio_set_pull_mode(PZEM_RX_PHASE_1, GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(PZEM_RX_PHASE_2, GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(PZEM_RX_PHASE_3, GPIO_PULLUP_ONLY);

  // Start with Phase-1 pin as default
  ret = uart_set_pin(PZEM_UART_PORT, PZEM_ESP_TX_PIN, PZEM_RX_PHASE_1,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "UART set pin failed: %s", esp_err_to_name(ret));
    return ret;
  }

  ESP_LOGI(TAG,
           "3-Phase PZEM-004T initialized. TX=GPIO%d, RX: P1=GPIO%d P2=GPIO%d "
           "P3=GPIO%d",
           PZEM_ESP_TX_PIN, PZEM_RX_PHASE_1, PZEM_RX_PHASE_2, PZEM_RX_PHASE_3);
  return ESP_OK;
}

// ─── Single Phase Read
// ────────────────────────────────────────────────────────
esp_err_t pzem_read(pzem_phase_t phase, pzem_data_t *data) {
  if (data == NULL || phase >= PZEM_PHASE_COUNT)
    return ESP_ERR_INVALID_ARG;

  data->valid = false;

  // ── Switch UART RX pin to this phase ──────────────────────────────────
  esp_err_t ret =
      uart_set_pin(PZEM_UART_PORT, PZEM_ESP_TX_PIN, phase_cfg[phase].rx_gpio,
                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "[%s] uart_set_pin failed", phase_cfg[phase].name);
    return ret;
  }

  // ── Settle: wait 100ms then flush to clear any glitch bytes ──────────
  vTaskDelay(pdMS_TO_TICKS(100));
  uart_flush_input(PZEM_UART_PORT);

  // ── Build Modbus RTU request ──────────────────────────────────────────
  uint8_t request[8];
  request[0] = phase_cfg[phase].modbus_addr;
  request[1] = 0x04; // Read Input Registers
  request[2] = 0x00; // Start Hi
  request[3] = 0x00; // Start Lo
  request[4] = 0x00; // Count Hi
  request[5] = 0x0A; // Count Lo = 10 registers
  uint16_t crc = crc16(request, 6);
  request[6] = (uint8_t)(crc & 0xFF);
  request[7] = (uint8_t)((crc >> 8) & 0xFF);

  // Flush once more right before sending — belt-and-suspenders
  uart_flush_input(PZEM_UART_PORT);
  int written =
      uart_write_bytes(PZEM_UART_PORT, (const char *)request, sizeof(request));
  if (written != (int)sizeof(request)) {
    ESP_LOGE(TAG, "[%s] UART write failed (%d/%d)", phase_cfg[phase].name,
             written, (int)sizeof(request));
    return ESP_FAIL;
  }

  // ── Read response (25 bytes expected) ─────────────────────────────────
  uint8_t response[25];
  int received = uart_read_bytes(PZEM_UART_PORT, response, sizeof(response),
                                 pdMS_TO_TICKS(1000)); // 1 second timeout

  if (received == 0) {
    ESP_LOGE(TAG, "[%s] Timeout (0 bytes)", phase_cfg[phase].name);
    return ESP_FAIL;
  }

  if (received < 25) {
    char hex[128] = {0};
    for (int i = 0; i < received && i < 20; i++) {
      sprintf(hex + strlen(hex), "%02X ", response[i]);
    }
    ESP_LOGW(TAG, "[%s] Partial (%d/25): %s", phase_cfg[phase].name, received,
             hex);
    return ESP_FAIL;
  }

  // ── Validate function code ────────────────────────────────────────────
  if (response[1] != 0x04) {
    ESP_LOGE(TAG, "[%s] Bad func code: 0x%02X", phase_cfg[phase].name,
             response[1]);
    return ESP_FAIL;
  }

  // ── CRC check ─────────────────────────────────────────────────────────
  uint16_t rx_crc = (uint16_t)response[24] << 8 | response[23];
  uint16_t calc_crc = crc16(response, 23);
  if (rx_crc != calc_crc) {
    ESP_LOGE(TAG, "[%s] CRC mismatch: got 0x%04X expected 0x%04X",
             phase_cfg[phase].name, rx_crc, calc_crc);
    return ESP_FAIL;
  }

  // ── Parse 10 registers ────────────────────────────────────────────────
  uint16_t reg[10];
  for (int i = 0; i < 10; i++) {
    reg[i] = (uint16_t)response[3 + i * 2] << 8 | response[3 + i * 2 + 1];
  }

  data->voltage = reg[0] / 10.0f;
  data->current = ((uint32_t)reg[2] << 16 | reg[1]) / 1000.0f;
  data->power = ((uint32_t)reg[4] << 16 | reg[3]) / 10.0f;
  data->energy = ((uint32_t)reg[6] << 16 | reg[5]) / 1000.0f;
  data->frequency = reg[7] / 10.0f;
  data->pf = reg[8] / 100.0f;
  data->alarm = (reg[9] == 0xFFFF) ? 1 : 0;
  data->valid = true;

  ESP_LOGI(TAG, "[%s] V=%.1fV I=%.3fA P=%.1fW E=%.3fkWh F=%.1fHz PF=%.2f",
           phase_cfg[phase].name, data->voltage, data->current, data->power,
           data->energy, data->frequency, data->pf);

  return ESP_OK;
}

// ─── Read All 3 Phases
// ────────────────────────────────────────────────────────
esp_err_t pzem_read_all(pzem_3phase_t *result) {
  if (result == NULL)
    return ESP_ERR_INVALID_ARG;

  memset(result, 0, sizeof(pzem_3phase_t));

  int valid_count = 0;

  for (int p = 0; p < PZEM_PHASE_COUNT; p++) {
    if (pzem_read((pzem_phase_t)p, &result->phase[p]) == ESP_OK) {
      valid_count++;
    }
  }

  return (valid_count > 0) ? ESP_OK : ESP_FAIL;
}
