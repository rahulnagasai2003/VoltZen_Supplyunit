#pragma once
/*
 * pzem004t.h — Supply Unit (3-Phase)
 *
 * Drives three PZEM-004T V3 modules on a single UART with one shared TX pin
 * and three separate RX pins (one per phase). The UART RX pin is switched
 * dynamically using uart_set_pin() before each phase read.
 *
 * Hardware wiring:
 *   ESP GPIO 17 (TX) ──► RX pin of PZEM Phase-1, Phase-2, Phase-3 (all three)
 *   PZEM Phase-1 TX  ──► ESP GPIO 16
 *   PZEM Phase-2 TX  ──► ESP GPIO 21
 *   PZEM Phase-3 TX  ──► ESP GPIO 22
 */

#include "driver/uart.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// ─── UART / Pin Configuration
// ─────────────────────────────────────────────────
#define PZEM_UART_PORT UART_NUM_2
#define PZEM_ESP_TX_PIN 17
#define PZEM_RX_PHASE_1 16
#define PZEM_RX_PHASE_2 21
#define PZEM_RX_PHASE_3 22
#define PZEM_BAUD_RATE 9600

// Since we have separate RX pins for each phase (hardware multiplexing),
// all PZEMs can safely share the identical address 0x01. When queried, they
// will all respond, but the ESP32 only listens to one RX pin at a time!
#define PZEM_ADDR_PHASE_1 0x01
#define PZEM_ADDR_PHASE_2 0x02
#define PZEM_ADDR_PHASE_3 0x03

// ─── Phase Selector
// ───────────────────────────────────────────────────────────
typedef enum {
  PZEM_PHASE_1 = 0,
  PZEM_PHASE_2,
  PZEM_PHASE_3,
  PZEM_PHASE_COUNT
} pzem_phase_t;

// ─── Per-Phase Data
// ───────────────────────────────────────────────────────────
typedef struct {
  float voltage;   // Volts
  float current;   // Amperes
  float power;     // Watts
  float energy;    // kWh
  float frequency; // Hz
  float pf;        // Power Factor (0.00–1.00)
  uint8_t alarm;   // 1 = over-energy alarm
  bool valid;      // true = data successfully read
} pzem_data_t;

// ─── 3-Phase Combined Result
// ──────────────────────────────────────────────────
typedef struct {
  pzem_data_t phase[PZEM_PHASE_COUNT];
  float total_power;
  float total_current;
  float total_energy;
  float avg_pf;
} pzem_3phase_t;

// ─── Public API
// ───────────────────────────────────────────────────────────────
esp_err_t pzem_init(void);
esp_err_t pzem_read(pzem_phase_t phase, pzem_data_t *data);
esp_err_t pzem_read_all(pzem_3phase_t *result);
