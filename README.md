# Supply Unit — ESP32 Firmware

## Overview
This is the **Supply Unit ESP32** firmware for the industrial oven monitoring system.
It monitors 3-phase power supply, manages WiFi provisioning, and relays oven data to the cloud.

---

## What this firmware does

| Feature | Details |
|---------|---------|
| **BLE Provisioning** | Advertises as `SupplyUnit_XXXX` on first boot. Use the **ESP BLE Provisioning** app to send WiFi SSID, password, and thermocouple type (`J`/`K`). Skipped on subsequent boots. |
| **3-Phase Power Monitoring** | Reads voltage, current, power, energy, frequency, and PF from 3× PZEM-004T modules via Modbus RTU. |
| **AWS IoT (MQTT)** | Publishes combined JSON (3-phase supply + oven status) to AWS IoT Core every **5 minutes**. |
| **ESP-NOW Master** | Communicates with the Oven Unit ESP32 — sends WiFi creds + thermocouple type on handshake, sends threshold updates, monitors oven liveness via 60-second heartbeats. |
| **Threshold Alerts** | Publishes alerts when power factor drops below threshold. Forwards temperature threshold to the Oven Unit. |
| **NVS Persistence** | All credentials and thresholds survive resets (stored in NVS flash). |

---

## Hardware Wiring

### PZEM-004T × 3 (3-Phase Power)

| Signal | ESP32 GPIO |
|--------|-----------|
| Shared TX (→ all PZEM RX) | **GPIO 17** |
| Phase-1 PZEM TX (→ ESP RX) | **GPIO 16** |
| Phase-2 PZEM TX (→ ESP RX) | **GPIO 21** |
| Phase-3 PZEM TX (→ ESP RX) | **GPIO 22** |

> ⚠️ **Voltage divider required** on each RX line (PZEM TX = 5V, ESP32 input = 3.3V max).
> Use a 10kΩ + 20kΩ divider.

### PZEM Modbus Address Programming (one-time setup)
Before first use, program each PZEM-004T with a unique Modbus address:
- Phase-1 → `0x01`
- Phase-2 → `0x02`
- Phase-3 → `0x03`

Use the PZEM PC software via USB-TTL adapter to set addresses.

---

## File Structure

```
supply unit/
├── CMakeLists.txt
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml
│   ├── certs/
│   │   ├── client_cert.pem     ← AWS IoT client certificate
│   │   ├── client_key.pem      ← AWS IoT private key
│   │   └── root_cert.pem       ← Amazon Root CA
│   ├── main.c                  ← Entry point and task orchestration
│   ├── ble_provisioning.c/.h   ← BLE WiFi provisioning (wifi_prov_mgr)
│   ├── nvs_manager.c/.h        ← NVS read/write for all persistent data
│   ├── wifi_manager.c/.h       ← WiFi STA connect + RSSI
│   ├── aws_iot.c/.h            ← AWS IoT MQTT publish/subscribe
│   ├── pzem004t.c/.h           ← 3-phase PZEM Modbus RTU driver
│   └── espnow_master.c/.h      ← ESP-NOW master (handshake, heartbeat, relay)
```

---

## AWS IoT Topics

| Topic | Direction | Content |
|-------|-----------|---------|
| `device/{mac}/telemetry` | → AWS | Full 3-phase + oven JSON every 5 min |
| `device/{mac}/alert` | → AWS | Threshold breach or oven offline |
| `device/{mac}/status` | → AWS | Online status on connect |
| `device/{mac}/threshold` | ← AWS | App sets PF thresholds + temp threshold |
| `device/{mac}/threshold/ack` | → AWS | Acknowledgement after saving thresholds |
| `device/{mac}/offline` | → AWS | LWT (Last Will) — published on disconnect |

> ⚠️ Topic names TBD — update `aws_iot.c` when confirmed with app team.

---

## Full Telemetry JSON

```json
{
  "device_id": "A4:CF:12:34:56:78",
  "timestamp": 1717582345,
  "pf": 0.97,
  "signal_strength": -65,
  "unit_supply": {
    "phase_1": { "voltage": 230.1, "current": 45.2, "power": 10400.5, "energy": 12500.2, "frequency": 50.1 },
    "phase_2": { "voltage": 229.5, "current": 44.8, "power": 10281.6, "energy": 12480.1, "frequency": 50.1 },
    "phase_3": { "voltage": 231.0, "current": 46.1, "power": 10649.1, "energy": 12550.8, "frequency": 50.1 }
  },
  "oven_status": {
    "oven_id": "RT_Oven_00001",
    "temperature": 245.5,
    "fan_1_current": 1.2,
    "fan_2_current": 1.1
  }
}
```

---

## Threshold JSON (from app to device)

```json
{
  "pf_high": 0.99,
  "pf_low": 0.85,
  "temp_threshold": 300.0
}
```

---

## ESP-NOW Messages (Supply ↔ Oven)

| Type | Code | Direction |
|------|------|-----------|
| HANDSHAKE_REQ | `0x01` | Oven → Supply |
| HANDSHAKE_ACK | `0x02` | Supply → Oven (sends WiFi + TC type + threshold) |
| OVEN_DATA | `0x03` | Oven → Supply |
| THRESHOLD_UPDATE | `0x04` | Supply → Oven |
| HEARTBEAT_REQ | `0x05` | Supply → Oven (every 60 s) |
| HEARTBEAT_ACK | `0x06` | Oven → Supply |

---

## Build and Flash

```bash
# Set IDF target
idf.py set-target esp32

# Build
idf.py build

# Flash and monitor
idf.py -p COM_PORT flash monitor
```

---

## AWS Certificates

> ⚠️ **Important**: The certificates in `main/certs/` are shared from the existing templyzen project for development.
> For production, register a new **AWS IoT Thing** for the supply unit and replace the `.pem` files.

1. Go to AWS IoT Console → Manage → Things → Create Thing
2. Download: `client_cert.pem`, `client_key.pem`, `root_cert.pem`
3. Replace files in `main/certs/`
4. Update `CLIENT_ID` in `aws_iot.c` to match your Thing name

---

## First Boot Sequence

1. Power on → No WiFi credentials in NVS → BLE advertises `SupplyUnit_XXXX`
2. Open **ESP BLE Provisioning** app → scan → connect
3. Send: WiFi SSID + Password + TC type (`K` or `J`)
4. Device connects to WiFi → MQTT to AWS → ESP-NOW ready
5. All subsequent reboots skip BLE provisioning automatically
