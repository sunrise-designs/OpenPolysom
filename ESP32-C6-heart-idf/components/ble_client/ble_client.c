#include "ble_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ble";

// ── Heap-event log (connect/disconnect free-heap samples) ───────────────────
// Diagnostic aid for a suspected BLE-stack memory leak across repeated
// reconnects — see ble_get_heap_log() in the header.
static ble_heap_event_t s_heap_log[BLE_HEAP_LOG_MAX];
static size_t           s_heap_log_count = 0;

static void log_heap_event(bool connected)
{
    if (s_heap_log_count >= BLE_HEAP_LOG_MAX) return;
    ble_heap_event_t *e = &s_heap_log[s_heap_log_count++];
    e->timestamp     = time(NULL);
    e->connected     = connected;
    e->free_heap     = esp_get_free_heap_size();
    e->min_free_heap = esp_get_minimum_free_heap_size();
}

size_t ble_get_heap_log(ble_heap_event_t *out, size_t max_out)
{
    size_t n = s_heap_log_count < max_out ? s_heap_log_count : max_out;
    memcpy(out, s_heap_log, n * sizeof(ble_heap_event_t));
    return n;
}

// ── State ─────────────────────────────────────────────────────────────────────
static volatile uint16_t s_bpm      = 0;
static volatile uint16_t s_rr_ms    = 0;
static volatile bool     s_connected = false;
static char s_dev_name[64]  = "Scanning...";
static char s_dev_addr[18]  = "";

static uint16_t s_conn_handle    = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_svc_end        = 0;
static uint16_t s_chr_val_handle = 0;

// Heart Rate Service / Characteristic UUIDs
static const ble_uuid16_t s_hr_svc_uuid = BLE_UUID16_INIT(0x180D);
static const ble_uuid16_t s_hr_chr_uuid = BLE_UUID16_INIT(0x2A37);

// Generic Access Service / Device Name characteristic (for a reliable,
// authoritative name — the advertising packet that triggers the connect
// often doesn't carry the name; it may only be in a separate scan-response
// packet that we never see because we cancel scanning as soon as the HR
// service UUID is found).
static const ble_uuid16_t s_gap_svc_uuid       = BLE_UUID16_INIT(0x1800);
static const ble_uuid16_t s_dev_name_chr_uuid  = BLE_UUID16_INIT(0x2A00);

// ── Public getters ────────────────────────────────────────────────────────────
uint16_t    ble_get_bpm(void)           { return s_bpm; }
uint16_t    ble_get_rr_ms(void)         { return s_rr_ms; }
bool        ble_is_connected(void)      { return s_connected; }
const char *ble_get_device_name(void)   { return s_dev_name; }
const char *ble_get_device_address(void){ return s_dev_addr; }
uint16_t    ble_get_hr_svc_uuid(void)   { return ble_uuid_u16(&s_hr_svc_uuid.u); }

// ── Forward declarations ──────────────────────────────────────────────────────
static void start_scan(void);
static int  on_gap_event(struct ble_gap_event *event, void *arg);
static void start_dev_name_disc(uint16_t conn_handle);

// ── HR notification parser ────────────────────────────────────────────────────
static void parse_hr_notify(struct os_mbuf *om)
{
    uint8_t buf[32];
    uint16_t len = OS_MBUF_PKTLEN(om);
    if (len < 2 || len > sizeof(buf)) return;
    os_mbuf_copydata(om, 0, len, buf);

    uint8_t flags = buf[0];
    int offset = 1;

    if (flags & 0x01) {
        // 16-bit BPM
        s_bpm = (uint16_t)buf[1] | ((uint16_t)buf[2] << 8);
        offset = 3;
    } else {
        // 8-bit BPM
        s_bpm  = buf[1];
        offset = 2;
    }
    if (flags & 0x08) offset += 2;  // skip energy expended field

    // RR intervals (1/1024 s units). The characteristic can carry more than one
    // RR value per notification (oldest first) when the heart rate exceeds the
    // BLE notify rate — walk all of them and keep the last (most recent) one,
    // rather than the first, so s_rr_ms never lags behind the latest beat.
    if (flags & 0x10) {
        while (offset + 1 < len) {
            uint16_t raw = (uint16_t)buf[offset] | ((uint16_t)buf[offset + 1] << 8);
            s_rr_ms = (uint16_t)((uint32_t)raw * 1000U / 1024U);
            offset += 2;
        }
    }
}

// ── GATT descriptor discovery → enable notifications ─────────────────────────
static int on_dsc_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                        uint16_t chr_def_handle, const struct ble_gatt_dsc *dsc,
                        void *arg)
{
    (void)chr_def_handle;
    (void)arg;
    if (error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGE(TAG, "Descriptor discovery failed: status=%d", error->status);
        return 0;
    }
    if (dsc == NULL) {
        if (error->status == BLE_HS_EDONE)
            ESP_LOGW(TAG, "Descriptor discovery done, no CCCD found on HR characteristic");
        return 0;
    }
    ESP_LOGI(TAG, "Found descriptor uuid=0x%04x handle=%d", ble_uuid_u16(&dsc->uuid.u), dsc->handle);

    // CCCD UUID = 0x2902
    if (ble_uuid_u16(&dsc->uuid.u) == 0x2902) {
        uint8_t cccd[2] = {0x01, 0x00};
        int rc = ble_gattc_write_flat(conn_handle, dsc->handle,
                                      cccd, sizeof(cccd), NULL, NULL);
        if (rc == 0)
            ESP_LOGI(TAG, "Notifications enabled on HR characteristic");
        else
            ESP_LOGE(TAG, "CCCD write failed: %d", rc);

        // Only look up the Device Name once HR notifications are set up:
        // NimBLE's GATT client has a small fixed pool of concurrent
        // procedures (CONFIG_BT_NIMBLE_GATT_MAX_PROCS, default 4). Starting
        // the name lookup earlier competed with HR service/characteristic/
        // descriptor discovery for those slots and could silently starve
        // the CCCD write, leaving BPM/RR stuck at 0.
        start_dev_name_disc(conn_handle);
    }
    return 0;
}

// ── Device Name read (from GATT, once connected) ─────────────────────────────
static int on_dev_name_read(uint16_t conn_handle, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (error->status != 0 || attr == NULL || attr->om == NULL) {
        ESP_LOGW(TAG, "Device Name read failed: status=%d", error->status);
        return 0;
    }
    uint16_t len = OS_MBUF_PKTLEN(attr->om);
    if (len >= sizeof(s_dev_name)) len = sizeof(s_dev_name) - 1;
    os_mbuf_copydata(attr->om, 0, len, s_dev_name);
    s_dev_name[len] = '\0';
    ESP_LOGI(TAG, "Device Name (GATT): %s", s_dev_name);
    return 0;
}

static int on_dev_name_chr_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                                 const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (error->status == BLE_HS_EDONE) return 0;
    if (error->status != 0) {
        ESP_LOGW(TAG, "Device Name characteristic discovery failed: status=%d", error->status);
        return 0;
    }
    if (chr == NULL) return 0;

    if (ble_uuid_u16(&chr->uuid.u) == 0x2A00) {
        int rc = ble_gattc_read(conn_handle, chr->val_handle, on_dev_name_read, NULL);
        if (rc != 0)
            ESP_LOGW(TAG, "Failed to start Device Name read: rc=%d", rc);
    }
    return 0;
}

// ── GATT characteristic discovery ────────────────────────────────────────────
static int on_chr_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                        const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (error->status == BLE_HS_EDONE) return 0;
    if (error->status != 0) {
        ESP_LOGE(TAG, "Characteristic discovery failed: status=%d", error->status);
        return 0;
    }
    if (chr == NULL) return 0;

    if (ble_uuid_u16(&chr->uuid.u) == 0x2A37) {
        s_chr_val_handle = chr->val_handle;
        ESP_LOGI(TAG, "HR Measurement chr: def_handle=%d val_handle=%d properties=0x%02x",
                 chr->def_handle, chr->val_handle, chr->properties);
        // NimBLE's Find-Information request actually starts at start_handle+1
        // internally, so pass val_handle itself (not val_handle+1) here.
        int rc = ble_gattc_disc_all_dscs(conn_handle,
                                          chr->val_handle, s_svc_end,
                                          on_dsc_disc, NULL);
        if (rc != 0)
            ESP_LOGE(TAG, "Failed to start descriptor discovery: rc=%d", rc);
    }
    return 0;
}

// ── GATT service discovery (HR service only) ─────────────────────────────────
static int on_svc_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                        const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;
    if (error->status == BLE_HS_EDONE) return 0;
    if (error->status != 0) {
        ESP_LOGE(TAG, "Service discovery failed: status=%d", error->status);
        return 0;
    }
    if (svc == NULL) return 0;

    s_svc_end = svc->end_handle;
    ESP_LOGI(TAG, "HR service: start_handle=%d end_handle=%d",
             svc->start_handle, svc->end_handle);
    int rc = ble_gattc_disc_chrs_by_uuid(conn_handle,
                                          svc->start_handle, svc->end_handle,
                                          (const ble_uuid_t *)&s_hr_chr_uuid,
                                          on_chr_disc, NULL);
    if (rc != 0)
        ESP_LOGE(TAG, "Failed to start characteristic discovery: rc=%d", rc);
    return 0;
}

// ── GATT service discovery (GAP service, for Device Name) ────────────────────
static int on_gap_svc_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                            const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;
    if (error->status == BLE_HS_EDONE) return 0;
    if (error->status != 0) {
        ESP_LOGW(TAG, "GAP service discovery failed: status=%d", error->status);
        return 0;
    }
    if (svc == NULL) return 0;

    ESP_LOGI(TAG, "GAP service: start_handle=%d end_handle=%d",
             svc->start_handle, svc->end_handle);
    int rc = ble_gattc_disc_chrs_by_uuid(conn_handle,
                                          svc->start_handle, svc->end_handle,
                                          (const ble_uuid_t *)&s_dev_name_chr_uuid,
                                          on_dev_name_chr_disc, NULL);
    if (rc != 0)
        ESP_LOGW(TAG, "Failed to start Device Name characteristic discovery: rc=%d", rc);
    return 0;
}

static void start_dev_name_disc(uint16_t conn_handle)
{
    int rc = ble_gattc_disc_svc_by_uuid(conn_handle,
                                         (const ble_uuid_t *)&s_gap_svc_uuid,
                                         on_gap_svc_disc, NULL);
    if (rc != 0)
        ESP_LOGW(TAG, "Failed to start GAP service discovery: rc=%d", rc);
}

// ── GAP event handler ─────────────────────────────────────────────────────────
static int on_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {

    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields,
                                     event->disc.data,
                                     event->disc.length_data) != 0) break;

        bool found = false;
        for (int i = 0; i < fields.num_uuids16; i++) {
            if (ble_uuid_cmp(&fields.uuids16[i].u, &s_hr_svc_uuid.u) == 0) {
                found = true;
                break;
            }
        }
        if (!found) break;

        ble_gap_disc_cancel();

        if (fields.name && fields.name_len > 0)
            snprintf(s_dev_name, sizeof(s_dev_name), "%.*s",
                     fields.name_len, fields.name);
        const uint8_t *a = event->disc.addr.val;
        snprintf(s_dev_addr, sizeof(s_dev_addr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 a[5], a[4], a[3], a[2], a[1], a[0]);

        ESP_LOGI(TAG, "Found HR sensor: %s [%s]", s_dev_name, s_dev_addr);

        ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &event->disc.addr,
                        30000, NULL, on_gap_event, NULL);
        break;
    }

    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            ESP_LOGE(TAG, "Connection failed (%d), restarting scan",
                     event->connect.status);
            start_scan();
            break;
        }
        s_conn_handle = event->connect.conn_handle;
        s_connected   = true;
        log_heap_event(true);
        ESP_LOGI(TAG, "Connected: %s", s_dev_name);
        int rc = ble_gattc_disc_svc_by_uuid(s_conn_handle,
                                             (const ble_uuid_t *)&s_hr_svc_uuid,
                                             on_svc_disc, NULL);
        if (rc != 0)
            ESP_LOGE(TAG, "Failed to start service discovery: rc=%d", rc);
        break;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        s_connected      = false;
        s_bpm            = 0;
        s_rr_ms          = 0;
        s_conn_handle    = BLE_HS_CONN_HANDLE_NONE;
        s_chr_val_handle = 0;
        log_heap_event(false);
        ESP_LOGI(TAG, "Disconnected (reason %d), restarting scan",
                 event->disconnect.reason);
        start_scan();
        break;

    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.attr_handle == s_chr_val_handle)
            parse_hr_notify(event->notify_rx.om);
        break;

    default:
        break;
    }
    return 0;
}

// ── Scan ──────────────────────────────────────────────────────────────────────
static void start_scan(void)
{
    struct ble_gap_disc_params p = {
        .itvl              = 0,
        .window            = 0,
        .filter_policy     = BLE_HCI_SCAN_FILT_NO_WL,
        .limited           = 0,
        .passive           = 0,
        .filter_duplicates = 1,
    };
    ESP_LOGI(TAG, "Scanning for Polar HR sensor...");
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &p,
                          on_gap_event, NULL);
    if (rc != 0) ESP_LOGE(TAG, "ble_gap_disc rc=%d", rc);
}

// ── NimBLE host task & init ───────────────────────────────────────────────────
static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset, reason=%d", reason);
}

static void on_sync(void)
{
    start_scan();
}

void ble_client_init(void)
{
    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.reset_cb      = on_reset;
    ble_hs_cfg.sync_cb       = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    nimble_port_freertos_init(ble_host_task);
}

// Mirrors the official stack_init_deinit() pattern in the IDF blecent
// example: nimble_port_stop() blocks (in this caller's task, not the host
// task) until the host's stop procedure has fully drained, at which point
// ble_host_task's nimble_port_run() returns and self-deletes via
// nimble_port_freertos_deinit() — safe to immediately follow with
// nimble_port_deinit() to release the controller/host heap.
void ble_client_deinit(void)
{
    int rc = nimble_port_stop();
    if (rc == 0) {
        nimble_port_deinit();
    } else {
        ESP_LOGW(TAG, "nimble_port_stop failed: %d", rc);
    }

    s_connected      = false;
    s_bpm            = 0;
    s_rr_ms          = 0;
    s_conn_handle    = BLE_HS_CONN_HANDLE_NONE;
    s_chr_val_handle = 0;
}
