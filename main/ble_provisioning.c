/*
 * ble_provisioning.c — Supply Unit (ESP-IDF v6, NimBLE GATT)
 *
 * Custom BLE GATT provisioning — replaces the removed wifi_provisioning component.
 *
 * ┌──────────────────────────────────────────────────────────────────────────┐
 * │  Service UUID : 4FAFC201-1FB5-459E-8FCC-C5C9C331914B                   │
 * │                                                                          │
 * │  Characteristics (all Write-without-response):                          │
 * │  ① SSID    BEB5483E-36E1-4688-B7F5-EA07361B26A8  ← write network name  │
 * │  ② Pass    BEB5483F-36E1-4688-B7F5-EA07361B26A9  ← write password       │
 * │  ③ Commit  BEB54841-36E1-4688-B7F5-EA07361B26AB  ← write "1" to save   │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * How to use (nRF Connect app on Android/iOS):
 *   1. Power on device → scan BLE → connect to "SupplyUnit_XXXX"
 *   2. Navigate to the custom service
 *   3. Write SSID → Write Password
 *   4. Write "1" to Commit → device saves and restarts Wi-Fi
 *
 * Compatible with any BLE terminal app (nRF Connect, LightBlue, etc.)
 */

#include "ble_provisioning.h"
#include "nvs_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>

/* NimBLE */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "BLE_PROV";

#define PROV_DONE_BIT BIT0

static EventGroupHandle_t s_prov_event_group = NULL;
static char               s_ssid[64]         = {0};
static char               s_pass[64]         = {0};

static char               s_device_name[32]  = "Rollin_VoltGen-X0009";

/* ── 128-bit UUID definitions (bytes in little-endian order) ─────────────────
 *
 *  Service  : 4FAFC201-1FB5-459E-8FCC-C5C9C331914B
 *  SSID     : BEB5483E-36E1-4688-B7F5-EA07361B26A8
 *  Password : BEB5483F-36E1-4688-B7F5-EA07361B26A9
 *  Commit   : BEB54841-36E1-4688-B7F5-EA07361B26AB
 */
static const ble_uuid128_t PROV_SVC_UUID = BLE_UUID128_INIT(
    0x4b,0x91,0x31,0xc3, 0xc9,0xc5, 0xcc,0x8f,
    0x9e,0x45, 0xb5,0x1f, 0x01,0xc2,0xaf,0x4f);

static const ble_uuid128_t SSID_CHR_UUID = BLE_UUID128_INIT(
    0xa8,0x26,0x1b,0x36, 0x07,0xea, 0xf5,0xb7,
    0x88,0x46, 0xe1,0x36, 0x3e,0x48,0xb5,0xbe);

static const ble_uuid128_t PASS_CHR_UUID = BLE_UUID128_INIT(
    0xa9,0x26,0x1b,0x36, 0x07,0xea, 0xf5,0xb7,
    0x88,0x46, 0xe1,0x36, 0x3f,0x48,0xb5,0xbe);

static const ble_uuid128_t COMMIT_CHR_UUID = BLE_UUID128_INIT(
    0xab,0x26,0x1b,0x36, 0x07,0xea, 0xf5,0xb7,
    0x88,0x46, 0xe1,0x36, 0x41,0x48,0xb5,0xbe);

/* ── GATT write callbacks ─────────────────────────────────────────────────── */

static int chr_ssid_cb(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len >= sizeof(s_ssid)) len = sizeof(s_ssid) - 1;
    os_mbuf_copydata(ctxt->om, 0, len, s_ssid);
    s_ssid[len] = '\0';
    ESP_LOGI(TAG, "SSID: %s", s_ssid);
    return 0;
}

static int chr_pass_cb(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len >= sizeof(s_pass)) len = sizeof(s_pass) - 1;
    os_mbuf_copydata(ctxt->om, 0, len, s_pass);
    s_pass[len] = '\0';
    ESP_LOGI(TAG, "Password received (len=%d)", len);
    return 0;
}

static int chr_commit_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    uint8_t val = 0;
    os_mbuf_copydata(ctxt->om, 0, 1, &val);
    if (val == '1' || val == 1) {
        ESP_LOGI(TAG, "Commit — saving SSID='%s'", s_ssid);
        nvs_save_wifi(s_ssid, s_pass);
        if (s_prov_event_group) {
            xEventGroupSetBits(s_prov_event_group, PROV_DONE_BIT);
        }
    }
    return 0;
}

/* ── GATT service table ───────────────────────────────────────────────────── */
static const struct ble_gatt_svc_def PROV_SVCS[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &PROV_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid      = &SSID_CHR_UUID.u,
                .access_cb = chr_ssid_cb,
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid      = &PASS_CHR_UUID.u,
                .access_cb = chr_pass_cb,
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid      = &COMMIT_CHR_UUID.u,
                .access_cb = chr_commit_cb,
                .flags     = BLE_GATT_CHR_F_WRITE,
            },
            { 0 } /* end characteristics */
        },
    },
    { 0 } /* end services */
};

/* ── BLE advertising ──────────────────────────────────────────────────────── */
static void ble_advertise(void)
{
    struct ble_gap_adv_params adv = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };

    struct ble_hs_adv_fields fields = {0};
    fields.flags                   = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name                    = (uint8_t *)s_device_name;
    fields.name_len                = strlen(s_device_name);
    fields.name_is_complete        = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields: %d", rc);
        return;
    }

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv, NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "BLE advertising as '%s'", s_device_name);
}

/* ── NimBLE host callbacks ────────────────────────────────────────────────── */
static void ble_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr: %d", rc);
        return;
    }

    /* Print own BLE address */
    ble_addr_t addr;
    rc = ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, addr.val, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "BLE addr: %02X:%02X:%02X:%02X:%02X:%02X",
                 addr.val[5], addr.val[4], addr.val[3],
                 addr.val[2], addr.val[1], addr.val[0]);
    }

    ble_advertise();
}

static void ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE host reset: %d", reason);
}

static void nimble_host_task(void *arg)
{
    nimble_port_run();          /* blocks until nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* ── Public API ───────────────────────────────────────────────────────────── */
bool ble_provisioning_start(char *ssid_out, char *pass_out)
{
    /* ── Skip if credentials already in NVS ── */
    char ssid[64] = {0};
    char pass[64] = {0};
    if (nvs_load_wifi(ssid, sizeof(ssid), pass, sizeof(pass))) {
        strncpy(ssid_out,    ssid, 63);
        strncpy(pass_out,    pass, 63);
        ESP_LOGI(TAG, "NVS credentials found — skipping BLE provisioning");
        return false;
    }

    ESP_LOGI(TAG, "No credentials — starting BLE GATT provisioning");
    s_prov_event_group = xEventGroupCreate();

    /* Fixed BLE device name */
    strncpy(s_device_name, "Rollin_VoltGen-X0009", sizeof(s_device_name) - 1);

    /* ── NimBLE init ── */
    nimble_port_init();

    ble_hs_cfg.sync_cb  = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;

    /* Register GATT services BEFORE nimble_port_freertos_init */
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(PROV_SVCS);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg: %d", rc);
        goto fail;
    }
    rc = ble_gatts_add_svcs(PROV_SVCS);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs: %d", rc);
        goto fail;
    }

    ble_svc_gap_device_name_set(s_device_name);

    /* Start the NimBLE host task (handles BLE events) */
    nimble_port_freertos_init(nimble_host_task);

    /* ── Log connection instructions ── */
    ESP_LOGI(TAG, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    ESP_LOGI(TAG, "BLE Provisioning Ready");
    ESP_LOGI(TAG, "Device Name : Rollin_VoltGen-X0009");
    ESP_LOGI(TAG, "Service     : 4FAFC201-1FB5-459E-8FCC-C5C9C331914B");
    ESP_LOGI(TAG, "① SSID     : BEB5483E-36E1-4688-B7F5-EA07361B26A8");
    ESP_LOGI(TAG, "② Password : BEB5483F-36E1-4688-B7F5-EA07361B26A9");
    ESP_LOGI(TAG, "③ Commit   : BEB54841-36E1-4688-B7F5-EA07361B26AB (write '1')");
    ESP_LOGI(TAG, "Use nRF Connect app to connect and write credentials");
    ESP_LOGI(TAG, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");

    /* ── Block until user commits credentials ── */
    xEventGroupWaitBits(s_prov_event_group, PROV_DONE_BIT,
                        pdFALSE, pdFALSE, portMAX_DELAY);

    vTaskDelay(pdMS_TO_TICKS(500));   /* let GATT response reach phone */

    /* ── Stop BLE ── */
    nimble_port_stop();
    vTaskDelay(pdMS_TO_TICKS(200));

fail:
    strncpy(ssid_out,    s_ssid,   63);
    strncpy(pass_out,    s_pass,   63);

    if (s_prov_event_group) {
        vEventGroupDelete(s_prov_event_group);
        s_prov_event_group = NULL;
    }

    ESP_LOGI(TAG, "Provisioning complete: SSID='%s'", ssid_out);
    return true;
}
