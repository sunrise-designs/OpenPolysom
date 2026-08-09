#include "rt_stream.h"
#include "config.h"
#include "logger.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "rt_stream";

// ── Frame geometry ───────────────────────────────────────────────────────────
// One frame is RT_STREAM_FRAME_MS of every channel. The 50 Hz count drives the
// frame boundary; ECG (100 Hz) fills at twice the rate, and RR (2.5 Hz) lands in
// roughly one frame in four.
#define FRAME_50HZ   (RT_STREAM_FRAME_MS * SENSOR_RATE_HZ / 1000)   // 5
#define FRAME_ECG    (RT_STREAM_FRAME_MS * ECG_RATE_HZ    / 1000)   // 10
#define FRAME_RR_MAX 2

// JSON for one frame: 9 fifty-hertz blocks (~55 B each), ECG (~90 B), RR, and
// the envelope. 2 KB leaves better than 2x headroom over the worst case.
#define JSON_BUF_SIZE 2048

// ── The sample frame handed from the sensor task to the streaming task ───────
// Plain values, no pointers: the queue copies by value, so a frame in flight is
// never aliased by the producer still filling the next one.
typedef struct {
    uint32_t seq;
    uint32_t n0_50;                    // absolute index of the first 50 Hz sample
    uint32_t n0_ecg;
    uint32_t n0_rr;
    uint8_t  n50, necg, nrr;
    int16_t  thoracic[FRAME_50HZ];
    int16_t  abdomen [FRAME_50HZ];
    int16_t  flow    [FRAME_50HZ];
    int16_t  a0x[FRAME_50HZ], a0y[FRAME_50HZ], a0z[FRAME_50HZ];
    int16_t  a1x[FRAME_50HZ], a1y[FRAME_50HZ], a1z[FRAME_50HZ];
    int16_t  ecg[FRAME_ECG];
    int16_t  rr [FRAME_RR_MAX];
} rt_frame_t;

static bool             s_enabled   = false;
static bool             s_up        = false;
static httpd_handle_t   s_httpd     = NULL;
static QueueHandle_t    s_queue     = NULL;
static volatile uint32_t s_dropped  = 0;
static volatile uint32_t s_clients  = 0;

// Producer-side state. Only ever touched from the sensor task, so it needs no
// lock — the queue is the handoff point.
static rt_frame_t s_fill;
static uint32_t   s_seq        = 0;
static uint32_t   s_count_50   = 0;   // absolute sample counters since streaming began
static uint32_t   s_count_ecg  = 0;
static uint32_t   s_count_rr   = 0;
static bool       s_fill_open  = false;

static char s_ssid[32];
static char s_password[16];

// ── NVS-persisted enable flag ────────────────────────────────────────────────
#define NVS_NAMESPACE "rt_stream"
#define NVS_KEY_ENABLED "enabled"

static bool load_enabled_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return RT_STREAM_ENABLED_DEFAULT;
    }
    uint8_t v = RT_STREAM_ENABLED_DEFAULT ? 1 : 0;
    if (nvs_get_u8(h, NVS_KEY_ENABLED, &v) != ESP_OK) {
        v = RT_STREAM_ENABLED_DEFAULT ? 1 : 0;
    }
    nvs_close(h);
    return v != 0;
}

void rt_stream_set_enabled(bool enabled)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed (%s); streaming setting not saved", esp_err_to_name(err));
        return;
    }
    nvs_set_u8(h, NVS_KEY_ENABLED, enabled ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
    // Deliberately not applied live. Bringing Wi-Fi up mid-recording would put
    // radio TX current and a new task in the path of the SD writes this
    // firmware exists to protect; a reboot is the safe moment to change it.
    ESP_LOGW(TAG, "Real-time streaming %s — takes effect on the next boot",
             enabled ? "ENABLED" : "DISABLED");
}

bool     rt_stream_is_enabled(void)   { return s_enabled; }
bool     rt_stream_is_up(void)        { return s_up; }
uint32_t rt_stream_client_count(void) { return s_clients; }
uint32_t rt_stream_dropped(void)      { return s_dropped; }
const char *rt_stream_ssid(void)      { return s_ssid; }
const char *rt_stream_password(void)  { return s_password; }

// ── Producer: called from the 100 Hz sensor task ─────────────────────────────

static void frame_reset(void)
{
    s_fill.n50 = s_fill.necg = s_fill.nrr = 0;
    s_fill.n0_50  = s_count_50;
    s_fill.n0_ecg = s_count_ecg;
    s_fill.n0_rr  = s_count_rr;
    s_fill_open   = true;
}

void rt_stream_push_ecg(uint16_t ecg_raw)
{
    if (!s_up || s_queue == NULL) return;
    if (!s_fill_open) frame_reset();

    if (s_fill.necg < FRAME_ECG) s_fill.ecg[s_fill.necg++] = (int16_t)ecg_raw;
    s_count_ecg++;
}

void rt_stream_push_50hz(int16_t  a0x, int16_t  a0y, int16_t  a0z,
                         int16_t  a1x, int16_t  a1y, int16_t  a1z,
                         uint32_t ldc0, uint32_t ldc1,
                         float    pressure_mbar,
                         uint16_t rr_ms)
{
    if (!s_up || s_queue == NULL) return;
    if (!s_fill_open) frame_reset();

    if (s_fill.n50 < FRAME_50HZ) {
        const uint8_t i = s_fill.n50;
        // Via logger's own conversions, so these are the integers that reach the
        // EDF+ — not a second derivation of them (see logger.h).
        s_fill.thoracic[i] = logger_thoracic_digital(ldc0);
        s_fill.abdomen [i] = logger_abdomen_digital(ldc1);
        s_fill.flow    [i] = logger_flow_digital(pressure_mbar);
        s_fill.a0x[i] = a0x; s_fill.a0y[i] = a0y; s_fill.a0z[i] = a0z;
        s_fill.a1x[i] = a1x; s_fill.a1y[i] = a1y; s_fill.a1z[i] = a1z;
        s_fill.n50++;
    }

    // RR is sampled every 20th 50 Hz tick (2.5 Hz), matching logger_record()'s
    // own cadence so the stream's RR indices line up with the EDF+'s.
    if ((s_count_50 % 20) == 19 && s_fill.nrr < FRAME_RR_MAX) {
        s_fill.rr[s_fill.nrr++] = (int16_t)rr_ms;
        s_count_rr++;
    }
    s_count_50++;

    if (s_fill.n50 >= FRAME_50HZ) {
        s_fill.seq = ++s_seq;
        // Zero timeout: the sample path must never wait on the radio. A full
        // queue means a client is not keeping up, and the right answer is to
        // lose the frame (the viewer draws a gap) rather than delay the SD
        // write this task also drives.
        if (xQueueSend(s_queue, &s_fill, 0) != pdTRUE) s_dropped++;
        s_fill_open = false;
    }
}

// ── JSON encoding ────────────────────────────────────────────────────────────
// snprintf into a static buffer on the streaming task's own stack-free path:
// no heap in the send loop, matching the rest of this firmware's discipline.

static int append(char *buf, int cap, int at, const char *fmt, ...)
{
    if (at >= cap) return at;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + at, (size_t)(cap - at), fmt, ap);
    va_end(ap);
    return n < 0 ? at : at + n;
}

static int append_block(char *buf, int cap, int at, bool *first,
                        const char *name, uint32_t n0, const int16_t *v, uint8_t n)
{
    if (n == 0) return at;
    at = append(buf, cap, at, "%s\"%s\":{\"n0\":%lu,\"v\":[",
                *first ? "" : ",", name, (unsigned long)n0);
    *first = false;
    for (uint8_t i = 0; i < n; i++) {
        at = append(buf, cap, at, i == 0 ? "%d" : ",%d", (int)v[i]);
    }
    return append(buf, cap, at, "]}");
}

static int encode_frame(char *buf, int cap, const rt_frame_t *f)
{
    bool first = true;
    int at = append(buf, cap, 0, "{\"type\":\"samples\",\"seq\":%lu,\"blocks\":{",
                    (unsigned long)f->seq);
    at = append_block(buf, cap, at, &first, "thoracic", f->n0_50,  f->thoracic, f->n50);
    at = append_block(buf, cap, at, &first, "abdomen",  f->n0_50,  f->abdomen,  f->n50);
    at = append_block(buf, cap, at, &first, "flow",     f->n0_50,  f->flow,     f->n50);
    at = append_block(buf, cap, at, &first, "ecg",      f->n0_ecg, f->ecg,      f->necg);
    at = append_block(buf, cap, at, &first, "accel0_x", f->n0_50,  f->a0x,      f->n50);
    at = append_block(buf, cap, at, &first, "accel0_y", f->n0_50,  f->a0y,      f->n50);
    at = append_block(buf, cap, at, &first, "accel0_z", f->n0_50,  f->a0z,      f->n50);
    at = append_block(buf, cap, at, &first, "accel1_x", f->n0_50,  f->a1x,      f->n50);
    at = append_block(buf, cap, at, &first, "accel1_y", f->n0_50,  f->a1y,      f->n50);
    at = append_block(buf, cap, at, &first, "accel1_z", f->n0_50,  f->a1z,      f->n50);
    at = append_block(buf, cap, at, &first, "rr",       f->n0_rr,  f->rr,       f->nrr);
    return append(buf, cap, at, "}}");
}

// One row per EDF+ signal, mirroring logger.cpp's SigDef table verbatim. The
// viewer builds its panes and its digital->physical scaling from this, so the
// two tables must stay in step — see wiki/knowledge/data-formats.md.
static const char *HELLO_CHANNELS =
    "["
    "{\"name\":\"thoracic\",\"edf_label\":\"Thoracic\",\"transducer\":\"LDC1612 CH0\",\"unit\":\"counts\",\"sample_rate_hz\":50,\"digital_min\":-32767,\"digital_max\":32767,\"physical_min\":-1000000,\"physical_max\":1000000},"
    "{\"name\":\"abdomen\",\"edf_label\":\"Abdomen\",\"transducer\":\"LDC1612 CH1\",\"unit\":\"counts\",\"sample_rate_hz\":50,\"digital_min\":-32767,\"digital_max\":32767,\"physical_min\":-1000000,\"physical_max\":1000000},"
    "{\"name\":\"flow\",\"edf_label\":\"Flow\",\"transducer\":\"SDP800-125Pa\",\"unit\":\"mbar\",\"sample_rate_hz\":50,\"digital_min\":-32767,\"digital_max\":32767,\"physical_min\":-100,\"physical_max\":100},"
    "{\"name\":\"ecg\",\"edf_label\":\"ECG\",\"transducer\":\"AD8232 ADC0\",\"unit\":\"ADC\",\"sample_rate_hz\":100,\"digital_min\":0,\"digital_max\":4095,\"physical_min\":0,\"physical_max\":4095},"
    "{\"name\":\"accel0_x\",\"edf_label\":\"Accel0X\",\"transducer\":\"MMA8451 ch0\",\"unit\":\"mg\",\"sample_rate_hz\":50,\"digital_min\":-8192,\"digital_max\":8191,\"physical_min\":-2000,\"physical_max\":2000},"
    "{\"name\":\"accel0_y\",\"edf_label\":\"Accel0Y\",\"transducer\":\"MMA8451 ch0\",\"unit\":\"mg\",\"sample_rate_hz\":50,\"digital_min\":-8192,\"digital_max\":8191,\"physical_min\":-2000,\"physical_max\":2000},"
    "{\"name\":\"accel0_z\",\"edf_label\":\"Accel0Z\",\"transducer\":\"MMA8451 ch0\",\"unit\":\"mg\",\"sample_rate_hz\":50,\"digital_min\":-8192,\"digital_max\":8191,\"physical_min\":-2000,\"physical_max\":2000},"
    "{\"name\":\"accel1_x\",\"edf_label\":\"Accel1X\",\"transducer\":\"MMA8451 ch1\",\"unit\":\"mg\",\"sample_rate_hz\":50,\"digital_min\":-8192,\"digital_max\":8191,\"physical_min\":-2000,\"physical_max\":2000},"
    "{\"name\":\"accel1_y\",\"edf_label\":\"Accel1Y\",\"transducer\":\"MMA8451 ch1\",\"unit\":\"mg\",\"sample_rate_hz\":50,\"digital_min\":-8192,\"digital_max\":8191,\"physical_min\":-2000,\"physical_max\":2000},"
    "{\"name\":\"accel1_z\",\"edf_label\":\"Accel1Z\",\"transducer\":\"MMA8451 ch1\",\"unit\":\"mg\",\"sample_rate_hz\":50,\"digital_min\":-8192,\"digital_max\":8191,\"physical_min\":-2000,\"physical_max\":2000},"
    "{\"name\":\"rr\",\"edf_label\":\"RR\",\"transducer\":\"N/A\",\"unit\":\"ms\",\"sample_rate_hz\":2.5,\"digital_min\":0,\"digital_max\":2000,\"physical_min\":0,\"physical_max\":2000}"
    "]";

static int encode_hello(char *buf, int cap)
{
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);

    // The stream's sample indices are counted from when streaming started, so
    // its time origin is that moment — which is the recording's own start when
    // both began together at boot, and is stated explicitly either way.
    time_t now = time(NULL) - (time_t)(s_count_50 / SENSOR_RATE_HZ);
    struct tm t;
    localtime_r(&now, &t);
    char start_iso[32];
    strftime(start_iso, sizeof(start_iso), "%Y-%m-%dT%H:%M:%S", &t);

    const char *tz = getenv("TZ");
    return snprintf(buf, (size_t)cap,
        "{\"type\":\"hello\",\"protocol\":\"protosom.rt/1.0.0\","
        "\"device_uid\":\"%02X%02X%02X%02X%02X%02X\","
        "\"recording_start_iso\":\"%s\",\"timezone\":\"%s\","
        "\"recording_active\":%s,\"record_duration_s\":%d,\"channels\":%s}",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        start_iso, tz ? tz : LOCAL_TZ,
        logger_is_active() ? "true" : "false",
        RECORD_DURATION_S, HELLO_CHANNELS);
}

static int encode_status(char *buf, int cap)
{
    return snprintf(buf, (size_t)cap,
        "{\"type\":\"status\",\"recording\":%s,\"elapsed_s\":%lu,\"batt_pct\":null,"
        "\"sd_error\":%s,\"dropped_frames\":%lu,\"clients\":%lu}",
        logger_is_active() ? "true" : "false",
        (unsigned long)logger_get_elapsed_seconds(),
        logger_had_write_error() ? "true" : "false",
        (unsigned long)s_dropped, (unsigned long)s_clients);
}

// ── WebSocket plumbing ───────────────────────────────────────────────────────

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        // The handshake itself; esp_http_server has already completed it.
        // Reply with `hello` so the client can build its panes immediately,
        // before the first sample frame arrives.
        static char hello[JSON_BUF_SIZE];
        int n = encode_hello(hello, sizeof(hello));
        if (n <= 0 || n >= (int)sizeof(hello)) {
            ESP_LOGE(TAG, "hello frame did not fit in %d bytes", (int)sizeof(hello));
            return ESP_FAIL;
        }
        httpd_ws_frame_t f = {
            .final = true, .fragmented = false, .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)hello, .len = (size_t)n,
        };
        ESP_LOGI(TAG, "client connected (fd %d)", httpd_req_to_sockfd(req));
        return httpd_ws_send_frame(req, &f);
    }
    // Nothing is expected from the client; drain and ignore whatever arrives so
    // a stray frame cannot wedge the connection.
    httpd_ws_frame_t in = { .type = HTTPD_WS_TYPE_TEXT };
    return httpd_ws_recv_frame(req, &in, 0);
}

/** Broadcast one text payload to every connected WebSocket client. */
static void broadcast(const char *payload, size_t len)
{
    if (s_httpd == NULL) return;

    size_t n_fds = RT_STREAM_AP_MAX_CONN + 2;
    int fds[RT_STREAM_AP_MAX_CONN + 2];
    if (httpd_get_client_list(s_httpd, &n_fds, fds) != ESP_OK) return;

    uint32_t live = 0;
    for (size_t i = 0; i < n_fds; i++) {
        if (httpd_ws_get_fd_info(s_httpd, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;
        live++;
        httpd_ws_frame_t f = {
            .final = true, .fragmented = false, .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)payload, .len = len,
        };
        // Async: a client that has stopped reading must not block the loop that
        // is also serving the others.
        esp_err_t err = httpd_ws_send_frame_async(s_httpd, fds[i], &f);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "send to fd %d failed (%s); closing", fds[i], esp_err_to_name(err));
            httpd_sess_trigger_close(s_httpd, fds[i]);
        }
    }
    s_clients = live;
}

static void rt_stream_task(void *arg)
{
    (void)arg;
    static char json[JSON_BUF_SIZE];
    rt_frame_t frame;
    TickType_t last_status = xTaskGetTickCount();
    TickType_t last_client = xTaskGetTickCount();

    for (;;) {
        if (xQueueReceive(s_queue, &frame, pdMS_TO_TICKS(RT_STREAM_FRAME_MS * 2)) == pdTRUE) {
            if (s_clients > 0) {
                int n = encode_frame(json, sizeof(json), &frame);
                if (n > 0 && n < (int)sizeof(json)) {
                    broadcast(json, (size_t)n);
                } else {
                    // Truncation would put malformed JSON on the wire, which the
                    // viewer would count as a bad frame. Drop it instead and say so.
                    ESP_LOGW(TAG, "frame %lu did not fit in %d bytes; dropped",
                             (unsigned long)frame.seq, (int)sizeof(json));
                    s_dropped++;
                }
            } else {
                // Nobody listening: still drain the queue, or the producer would
                // count every frame as a drop.
                broadcast("", 0);
            }
        }

        TickType_t now = xTaskGetTickCount();
        if (now - last_status >= pdMS_TO_TICKS(1000)) {
            last_status = now;
            int n = encode_status(json, sizeof(json));
            if (n > 0 && n < (int)sizeof(json) && s_clients > 0) broadcast(json, (size_t)n);
        }

        if (s_clients > 0) {
            last_client = now;
        } else if (now - last_client >= pdMS_TO_TICKS(RT_STREAM_IDLE_TIMEOUT_S * 1000)) {
            // Nothing has watched for a while: give the power back. The
            // recording continues untouched; a reboot is needed to stream again.
            ESP_LOGW(TAG, "No client for %d s — shutting the radio down",
                     RT_STREAM_IDLE_TIMEOUT_S);
            s_up = false;
            httpd_stop(s_httpd);
            s_httpd = NULL;
            esp_wifi_stop();
            esp_wifi_deinit();
            vTaskDelete(NULL);
            return;
        }
    }
}

// ── Wi-Fi bring-up ───────────────────────────────────────────────────────────

static void derive_credentials(void)
{
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(s_ssid, sizeof(s_ssid), RT_STREAM_SSID_PREFIX "%02X%02X%02X", mac[3], mac[4], mac[5]);
    // WPA2 needs >= 8 characters. Derived from the MAC rather than hardcoded, so
    // units differ; shown on the OLED, so it is discoverable without a manual.
    // Not a secret in any strong sense — it keeps live physiological data off a
    // casual passer-by's phone, which is what an open AP would not do.
    snprintf(s_password, sizeof(s_password), RT_STREAM_PASSWORD_PREFIX "%02X%02X%02X",
             mac[3], mac[4], mac[5]);
}

static bool wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

#if RT_STREAM_SOFTAP
    esp_netif_create_default_wifi_ap();
    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, s_ssid, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len       = (uint8_t)strlen(s_ssid);
    ap.ap.channel        = RT_STREAM_AP_CHANNEL;
    ap.ap.max_connection = RT_STREAM_AP_MAX_CONN;
    ap.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    strncpy((char *)ap.ap.password, s_password, sizeof(ap.ap.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
#else
    esp_netif_create_default_wifi_sta();
    wifi_config_t sta = {0};
    strncpy((char *)sta.sta.ssid, RT_STREAM_STA_SSID, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, RT_STREAM_STA_PASSWORD, sizeof(sta.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
#endif

    // Modem sleep between beacons. The radio is the reason this feature is
    // opt-in; take back what can be taken back without dropping the link.
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return false;
    }
#if !RT_STREAM_SOFTAP
    esp_wifi_connect();
#endif
    return true;
}

static bool httpd_start_ws(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = RT_STREAM_PORT;
    cfg.max_open_sockets = RT_STREAM_AP_MAX_CONN + 1;
    cfg.lru_purge_enable = true;

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return false;
    }
    static const httpd_uri_t ws_uri = {
        .uri          = RT_STREAM_WS_PATH,
        .method       = HTTP_GET,
        .handler      = ws_handler,
        .user_ctx     = NULL,
        .is_websocket = true,
    };
    httpd_register_uri_handler(s_httpd, &ws_uri);
    return true;
}

void rt_stream_start(void)
{
    s_enabled = load_enabled_from_nvs();
    if (!s_enabled) {
        ESP_LOGI(TAG, "Real-time streaming disabled (radio stays off; "
                      "enable with tools/set_rt_stream.py)");
        return;
    }

    derive_credentials();

    s_queue = xQueueCreate(RT_STREAM_QUEUE_FRAMES, sizeof(rt_frame_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "Could not allocate the frame queue; streaming off");
        return;
    }

    if (!wifi_start() || !httpd_start_ws()) {
        ESP_LOGE(TAG, "Streaming bring-up failed; recording continues without it");
        return;
    }

    // Below the sensor task's priority: acquisition and the SD write always win.
    if (xTaskCreate(rt_stream_task, "rt_stream", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not start the streaming task");
        httpd_stop(s_httpd);
        s_httpd = NULL;
        return;
    }

    s_up = true;
#if RT_STREAM_SOFTAP
    ESP_LOGW(TAG, "Streaming live on SoftAP \"%s\" (pass \"%s\") — "
                  "open the viewer at index.html?rt=192.168.4.1",
             s_ssid, s_password);
#else
    ESP_LOGW(TAG, "Streaming live; joining \"%s\"", RT_STREAM_STA_SSID);
#endif
}
