#include "logger.h"
#include "config.h"
#include "edflib.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_vfs_fat.h"
#include "driver/spi_master.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sensors.h"
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <math.h>
#include <stdlib.h>
#include <sys/stat.h>

static const char *TAG = "logger";

// ── ESP_LOG capture to SD ──────────────────────────────────────────────────────
// Buffers formatted log lines in RAM and only touches the SD card when the
// buffer fills (or at existing safe flush points), to keep write wear low.
// Warnings and errors are written through immediately — they are rare, and
// they are the lines worth spending a write on.
#define LOG_BUF_SIZE 4096
static char             log_buf[LOG_BUF_SIZE];
static size_t           log_buf_len = 0;
static FILE            *log_file = NULL;
static char             log_file_path[80];
static SemaphoreHandle_t log_mutex = NULL;
static vprintf_like_t   prev_vprintf = NULL;

// Task currently inside log_mutex, or NULL. log_flush_locked() does SD I/O
// while holding the mutex, and the SD/FATFS driver logs its own errors when a
// write fails — so a flush can be re-entered by the very task performing it,
// on exactly the failure path this capture exists to record. log_mutex is
// non-recursive, so that nested line would otherwise block the task for the
// full take-timeout and be dropped anyway. Recognise ourselves and bail early.
static volatile TaskHandle_t log_owner = NULL;

// Caller must hold log_mutex.
static void log_flush_locked(void)
{
    if (log_buf_len == 0 || log_file == NULL) return;
    fwrite(log_buf, 1, log_buf_len, log_file);
    fflush(log_file);
    fsync(fileno(log_file));
    log_buf_len = 0;
}

// ESP_LOG's format string opens with the level letter ("E (123) tag: ..."),
// behind an ANSI colour escape when CONFIG_LOG_COLORS is on (it currently
// isn't, but skipping the escape keeps this working if it's ever enabled).
static bool log_line_is_urgent(const char *line)
{
    const char *p = line;
    if (*p == '\033') {
        while (*p && *p != 'm') p++;
        if (*p == 'm') p++;
    }
    return *p == 'E' || *p == 'W';
}

static int log_capture_vprintf(const char *fmt, va_list args)
{
    va_list args_copy;
    va_copy(args_copy, args);
    int console_n = prev_vprintf ? prev_vprintf(fmt, args) : vprintf(fmt, args);

    // Some driver logs (e.g. Wi-Fi MAC trace/ADDBA messages) are emitted from ISR
    // context. xSemaphoreTake/Give below are not ISR-safe, so skip SD buffering
    // there entirely rather than corrupting the interrupt stack.
    if (xPortInIsrContext()) {
        va_end(args_copy);
        return console_n;
    }

    // Nested log from inside our own flush (see log_owner) — console only.
    if (log_owner == xTaskGetCurrentTaskHandle()) {
        va_end(args_copy);
        return console_n;
    }

    char line[256];
    int n = vsnprintf(line, sizeof(line), fmt, args_copy);
    va_end(args_copy);
    if (n > 0) {
        if ((size_t)n >= sizeof(line)) n = sizeof(line) - 1; // truncated, still buffer what we have

        if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            log_owner = xTaskGetCurrentTaskHandle();
            if (log_buf_len + (size_t)n > LOG_BUF_SIZE) {
                log_flush_locked(); // no-op if log_file isn't open yet (e.g. no SD card)
            }
            // Recheck after the flush attempt: if there's still no file to drain
            // to, drop this line rather than writing past the end of log_buf.
            if (log_buf_len + (size_t)n <= LOG_BUF_SIZE) {
                memcpy(log_buf + log_buf_len, line, n);
                log_buf_len += n;
            }
            // The 10-record cadence in logger_record() leaves up to 100 s of log
            // in RAM, so a fault that stops the firmware before the next flush
            // takes its own explanation down with it. Warnings and errors are
            // the lines that explain a fault: write them through now.
            if (log_line_is_urgent(line)) {
                log_flush_locked();
            }
            log_owner = NULL;
            xSemaphoreGive(log_mutex);
        }
    }
    return console_n;
}

// Call once, first thing in app_main, so boot-time logs (time sync, etc.) are
// buffered even before the SD card is mounted.
void logger_log_init(void)
{
    log_mutex    = xSemaphoreCreateMutex();
    prev_vprintf = esp_log_set_vprintf(log_capture_vprintf);
}

static void log_flush(void)
{
    if (!log_mutex) return;
    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        log_owner = xTaskGetCurrentTaskHandle();
        log_flush_locked();
        log_owner = NULL;
        xSemaphoreGive(log_mutex);
    }
}

// Opens the SD log file (same timestamp as the EDF file) and flushes
// whatever was buffered during boot before the card was mounted.
static void log_open_file(void)
{
    if (!log_mutex) return;
    bool opened = false;
    if (xSemaphoreTake(log_mutex, portMAX_DELAY) == pdTRUE) {
        log_owner = xTaskGetCurrentTaskHandle();
        log_file = fopen(log_file_path, "w");
        opened = (log_file != NULL);
        log_flush_locked();
        log_owner = NULL;
        xSemaphoreGive(log_mutex);
    }
    // Logged outside the lock: ESP_LOGx re-enters log_capture_vprintf, which
    // would otherwise try to re-take this same non-recursive mutex.
    if (!opened) {
        ESP_LOGE(TAG, "Failed to open log file %s", log_file_path);
    }
}

static void log_close_file(void)
{
    if (!log_mutex) return;
    if (xSemaphoreTake(log_mutex, portMAX_DELAY) == pdTRUE) {
        log_owner = xTaskGetCurrentTaskHandle();
        log_flush_locked();
        if (log_file) {
            fclose(log_file);
            log_file = NULL;
        }
        log_owner = NULL;
        xSemaphoreGive(log_mutex);
    }
}

// ── EDF signal indices ────────────────────────────────────────────────────────
#define SIG_THORACIC 0
#define SIG_ABDOMEN  1
#define SIG_FLOW     2
#define SIG_ECG      3
#define SIG_A0X      4
#define SIG_A0Y      5
#define SIG_A0Z      6
#define SIG_A1X      7
#define SIG_A1Y      8
#define SIG_A1Z      9
#define SIG_RR      10
#define NUM_SIGNALS 11

// Raw-LDC1612-count-to-digital-sample divisors for the Thoracic/Abdomen RIP
// belts, chosen per-channel so each channel's breathing swing uses more of
// the 16-bit digital range (EDF physical range stays ±1,000,000 nH — see
// wiki open fork O3 "RIP physical-range fix"). Values are derived from one
// night's peak non-clipped swing with ~2x headroom; deeper breathing on a
// different patient/night could still saturate and needs re-tuning.
#define THORACIC_DIVISOR 15
#define ABDOMEN_DIVISOR  11

static int edf_handle = -1;
static bool logging_active = false;
static bool write_failed = false;
static SemaphoreHandle_t edf_mutex = NULL;
static uint32_t record_count = 0;
static char edf_file_path[80];
static char json_file_path[80];
static time_t recording_start_time = 0;

// ── Sample buffers ────────────────────────────────────────────────────────────
static int thoracic_buf[SAMPLES_50HZ];
static int abdomen_buf[SAMPLES_50HZ];
static int flow_buf[SAMPLES_50HZ];
static int ecg_buf[SAMPLES_100HZ];
static int a0x_buf[SAMPLES_50HZ], a0y_buf[SAMPLES_50HZ], a0z_buf[SAMPLES_50HZ];
static int a1x_buf[SAMPLES_50HZ], a1y_buf[SAMPLES_50HZ], a1z_buf[SAMPLES_50HZ];
static int rr_buf[SAMPLES_RR_HZ];

static int sample_idx = 0;
static int rr_idx     = 0;
static int ecg_idx    = 0;

// ── LDC baseline ──────────────────────────────────────────────────────────────
// Baseline is captured 20 minutes into the recording (BASELINE_DELAY_S), once
// belt-donning/posture-finding "gross inductance change" has settled, then
// averaged over 1 s (BASELINE_AVG_SAMPLES) to smooth sensor noise — mirroring
// the RPi5 main.cpp pattern (10-minute mark, 1 s average). Until then,
// thoracic/abdomen samples are written against a zero baseline (raw counts,
// effectively clipped/uncalibrated) — accepted drift-over-no-data tradeoff,
// same as main.cpp.
#define BASELINE_DELAY_S     1200
#define BASELINE_AVG_SAMPLES 50
static uint32_t ldc0_baseline   = 0;
static uint32_t ldc1_baseline   = 0;
static bool     baseline_ok     = false;
static uint64_t baseline_sum0   = 0;
static uint64_t baseline_sum1   = 0;
static int      baseline_avg_n  = 0;

uint32_t logger_get_ldc0_baseline(void) { return ldc0_baseline; }
uint32_t logger_get_ldc1_baseline(void) { return ldc1_baseline; }
bool     logger_get_baseline_ok(void)   { return baseline_ok; }
bool     logger_is_active(void)         { return logging_active; }
bool     logger_had_write_error(void)   { return write_failed; }
uint32_t logger_get_elapsed_seconds(void)
{
    return logging_active ? (uint32_t)difftime(time(NULL), recording_start_time) : 0;
}

// ── SD card ───────────────────────────────────────────────────────────────────
static sdmmc_card_t *s_card = NULL;

// SD card is now the sole user of this SPI bus — the LCD used to also share
// it, but the display moved to I2C (SH1106 OLED), so logger owns the bus.
static bool s_spi_bus_ready = false;

static bool sd_mount(void)
{
    if (!s_spi_bus_ready) {
        spi_bus_config_t bus_cfg = {};
        bus_cfg.mosi_io_num     = SPI_MOSI_PIN;
        bus_cfg.miso_io_num     = SPI_MISO_PIN;
        bus_cfg.sclk_io_num     = SPI_CLK_PIN;
        bus_cfg.quadwp_io_num   = -1;
        bus_cfg.quadhd_io_num   = -1;
        bus_cfg.max_transfer_sz = 4000;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST_ID, &bus_cfg, SPI_DMA_CH_AUTO));
        s_spi_bus_ready = true;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI_HOST_ID;
    host.max_freq_khz = SD_SPI_FREQ / 1000;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SD_CS_PIN;
    slot.host_id = SPI_HOST_ID;

    esp_vfs_fat_sdmmc_mount_config_t mnt = {};
    mnt.format_if_mount_failed = false;
    mnt.max_files              = 4;
    mnt.allocation_unit_size   = 16 * 1024;

    esp_err_t r = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mnt, &s_card);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(r));
        return false;
    }
    ESP_LOGI(TAG, "SD mounted: %s %.0f MB",
             s_card->cid.name,
             (float)((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) / 1048576.0f);
    return true;
}

// ── JSON sidecar ─────────────────────────────────────────────────────────────
// Written at recording start (end_t == NULL: recording_end_time is null) and
// rewritten in full at logger_close() (end_t != NULL: fills in
// recording_end_time).
static void write_json_sidecar(const struct tm *start_t, const struct tm *end_t)
{
    FILE *f = fopen(json_file_path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open sidecar file %s", json_file_path);
        return;
    }

    uint8_t mac[6] = {};
    esp_efuse_mac_get_default(mac);
    char device_uid[13];
    snprintf(device_uid, sizeof(device_uid), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    char start_time[32];
    strftime(start_time, sizeof(start_time), "%Y-%m-%dT%H:%M:%S", start_t);

    typedef struct { const char *name; bool present; double sample_rate_hz; } SensorInfo;
    const SensorInfo sensors[] = {
        {"MMA8451 accel0",             g_mma0_present,      50.0},
        {"MMA8451 accel1",             g_mma1_present,      50.0},
        {"LDC1612 (thoracic/abdomen)", g_ldc_present,       50.0},
        {"SDP800 flow",                g_sdp_present,       50.0},
        {"AD8232 ECG",                 true,               100.0},
        {"DS3231 RTC",                 g_rtc_present,        0.0},
    };
    const int num_sensors = sizeof(sensors) / sizeof(sensors[0]);

    const char *current_tz = getenv("TZ");
    fprintf(f, "{\n");
    fprintf(f, "  \"device_uid\": \"%s\",\n", device_uid);
    fprintf(f, "  \"recording_start_time\": \"%s\",\n", start_time);
    fprintf(f, "  \"timezone\": \"%s\",\n", current_tz ? current_tz : LOCAL_TZ);
    if (end_t) {
        char end_time[32];
        strftime(end_time, sizeof(end_time), "%Y-%m-%dT%H:%M:%S", end_t);
        fprintf(f, "  \"recording_end_time\": \"%s\",\n", end_time);
    } else {
        fprintf(f, "  \"recording_end_time\": null,\n");
    }
    fprintf(f, "  \"sensors\": [\n");
    for (int i = 0; i < num_sensors; i++) {
        fprintf(f, "    {\"name\": \"%s\", \"present\": %s, \"sample_rate_hz\": ",
                sensors[i].name, sensors[i].present ? "true" : "false");
        if (sensors[i].sample_rate_hz > 0) {
            fprintf(f, "%.0f}", sensors[i].sample_rate_hz);
        } else {
            fprintf(f, "null}");
        }
        fprintf(f, "%s\n", (i == num_sensors - 1) ? "" : ",");
    }
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");
    fclose(f);
    ESP_LOGI(TAG, "Sidecar written: %s", json_file_path);
}

// ── EDF setup ────────────────────────────────────────────────────────────────
// The timestamp stem is not unique on its own: with no time source the clock
// reads 1970-01-01T00:00:00 on every boot, so a reboot mid-study would rebuild
// the same three paths — and both fopen(..., "w") here and edflib's
// fopen(..., "wb") truncate, silently destroying the night's recording. Probe
// for a stem whose .edf does not exist yet, so an existing recording is never
// overwritten whatever the clock says. The unsuffixed name is tried first, so
// a run with a working clock keeps today's exact filename.
static bool build_unique_paths(const struct tm *t)
{
    for (int seq = 0; seq < 1000; seq++) {
        char stem[64];
        int n = snprintf(stem, sizeof(stem),
                         EDF_FILE_DIR "/" EDF_FILE_PREFIX "_%04d-%02d-%02d_%02d-%02d-%02d",
                         t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                         t->tm_hour, t->tm_min, t->tm_sec);
        if (seq > 0) snprintf(stem + n, sizeof(stem) - n, "_%03d", seq);

        snprintf(edf_file_path, sizeof(edf_file_path), "%s.edf", stem);

        struct stat st;
        if (stat(edf_file_path, &st) == 0) continue;  // taken — try the next seq

        snprintf(log_file_path,  sizeof(log_file_path),  "%s.log",  stem);
        snprintf(json_file_path, sizeof(json_file_path), "%s.json", stem);
        if (seq > 0) {
            ESP_LOGW(TAG, "Recording paths for this timestamp exist; using suffix _%03d", seq);
        }
        return true;
    }
    ESP_LOGE(TAG, "No free recording filename for this timestamp (1000 taken)");
    return false;
}

static bool open_edf(void)
{
    struct tm t = {};
    time_t now = time(NULL);
    localtime_r(&now, &t);
    if (!build_unique_paths(&t)) return false;
    log_open_file();

    edf_handle = edfopen_file_writeonly(edf_file_path,
                                        EDFLIB_FILETYPE_EDFPLUS, NUM_SIGNALS);
    if (edf_handle < 0) {
        ESP_LOGE(TAG, "edfopen failed: %d", edf_handle);
        return false;
    }

    // 10-second data records (1 000 000 × 10 µs)
    edf_set_datarecord_duration(edf_handle, 1000000);

    typedef struct { const char *label, *transducer, *dim; double rate; int dmax, dmin;
                     double pmax, pmin; } SigDef;
    static const SigDef sigs[NUM_SIGNALS] = {
        {"Thoracic","LDC1612 CH0",  "counts",50, 32767,-32767,  1e6, -1e6},
        {"Abdomen", "LDC1612 CH1",  "counts",50, 32767,-32767,  1e6, -1e6},
        {"Flow",    "SDP800-125Pa", "mbar",  50, 32767,-32767,  100.0, -100.0},
        {"ECG",     "AD8232 ADC0",  "ADC",  100,  4095,     0, 4095.0, 0.0},
        {"Accel0X", "MMA8451 ch0",  "mg",    50,  8191, -8192, 2000.0,-2000.0},
        {"Accel0Y", "MMA8451 ch0",  "mg",    50,  8191, -8192, 2000.0,-2000.0},
        {"Accel0Z", "MMA8451 ch0",  "mg",    50,  8191, -8192, 2000.0,-2000.0},
        {"Accel1X", "MMA8451 ch1",  "mg",    50,  8191, -8192, 2000.0,-2000.0},
        {"Accel1Y", "MMA8451 ch1",  "mg",    50,  8191, -8192, 2000.0,-2000.0},
        {"Accel1Z", "MMA8451 ch1",  "mg",    50,  8191, -8192, 2000.0,-2000.0},
        {"RR",      "N/A",         "ms",   2.5,  2000,     0, 2000.0, 0.0},
    };

    for (int i = 0; i < NUM_SIGNALS; i++) {
        edf_set_label(edf_handle, i, sigs[i].label);
        edf_set_transducer(edf_handle, i, sigs[i].transducer);
        // edf_set_samplefrequency() actually takes samples-per-datarecord, not Hz;
        // effective rate = samplefrequency / datarecord duration (edflib.h).
        // lround() guards the RR channel's fractional 2.5 Hz (25 samples/record)
        // against float truncation landing on 24.
        edf_set_samplefrequency(edf_handle, i, (int)lround(sigs[i].rate * RECORD_DURATION_S));
        edf_set_digital_maximum(edf_handle, i, sigs[i].dmax);
        edf_set_digital_minimum(edf_handle, i, sigs[i].dmin);
        edf_set_physical_maximum(edf_handle, i, sigs[i].pmax);
        edf_set_physical_minimum(edf_handle, i, sigs[i].pmin);
        edf_set_physical_dimension(edf_handle, i, sigs[i].dim);
    }

    edf_set_startdatetime(edf_handle,
                          t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                          t.tm_hour, t.tm_min, t.tm_sec);

    write_json_sidecar(&t, NULL);

    sample_idx    = 0;
    rr_idx        = 0;
    ecg_idx       = 0;
    record_count  = 0;
    write_failed  = false;
    recording_start_time = now;
    logging_active = true;
    ESP_LOGI(TAG, "EDF recording started: %s", edf_file_path);
    return true;
}

bool logger_init(void)
{
    if (edf_mutex == NULL) {
        edf_mutex = xSemaphoreCreateMutex();
    }
    if (!sd_mount()) return false;
    return open_edf();
}

// Writes one complete EDF data record: one call per signal, in the SIG_* order
// declared in open_edf(). Caller must hold edf_mutex.
//
// These returns must not be ignored: a card that stops accepting writes is
// otherwise invisible, and the recording ends at the last flushed record with
// nothing to say why. A failure part-way through also leaves the record short
// while edflib holds signal_write_sequence_pos at the failed signal, so the
// interleaving cannot be resumed at a known offset — the caller stops rather
// than append misaligned data behind a header that claims otherwise.
static bool write_record_locked(void)
{
    int *const bufs[NUM_SIGNALS] = {
        thoracic_buf, abdomen_buf, flow_buf, ecg_buf,
        a0x_buf, a0y_buf, a0z_buf,
        a1x_buf, a1y_buf, a1z_buf,
        rr_buf,
    };

    for (int i = 0; i < NUM_SIGNALS; i++) {
        if (edfwrite_digital_samples(edf_handle, bufs[i]) != 0) {
            ESP_LOGE(TAG, "EDF write failed: signal %d of record %lu (%lu records safe on card)",
                     i, (unsigned long)record_count + 1, (unsigned long)(record_count / 10) * 10);
            return false;
        }
    }
    return true;
}

// Caller must hold edf_mutex. Leaves the EDF handle open so logger_close() can
// still run, but stops the 50 Hz path from hammering a card that is not
// accepting writes. Everything up to the last edfflush_file() is valid on disk:
// the header's record count is what a reader trusts, and it is only ever
// advanced by a flush that succeeded.
static void stop_on_write_error_locked(void)
{
    logging_active = false;
    write_failed   = true;
    ESP_LOGE(TAG, "Recording stopped after SD write failure at %.1f min",
             (float)record_count * RECORD_DURATION_S / 60.0f);
}

void logger_record(int16_t  a0x, int16_t  a0y, int16_t  a0z,
                   int16_t  a1x, int16_t  a1y, int16_t  a1z,
                   uint32_t ldc0, uint32_t ldc1,
                   float    pressure_mbar,
                   uint16_t rr_ms)
{
    if (edf_mutex) xSemaphoreTake(edf_mutex, portMAX_DELAY);
    if (!logging_active || edf_handle < 0) {
        if (edf_mutex) xSemaphoreGive(edf_mutex);
        return;
    }

    if (!baseline_ok && ldc0 != 0 &&
        difftime(time(NULL), recording_start_time) >= BASELINE_DELAY_S) {
        baseline_sum0 += ldc0;
        baseline_sum1 += ldc1;
        baseline_avg_n++;
        if (baseline_avg_n >= BASELINE_AVG_SAMPLES) {
            ldc0_baseline = (uint32_t)(baseline_sum0 / baseline_avg_n);
            ldc1_baseline = (uint32_t)(baseline_sum1 / baseline_avg_n);
            baseline_ok   = true;
        }
    }

    int32_t d0 = (int32_t)(ldc0 - ldc0_baseline);
    int32_t d1 = (int32_t)(ldc1 - ldc1_baseline);

    int clamp = (d0 / THORACIC_DIVISOR < -32767) ? -32767 : (d0 / THORACIC_DIVISOR > 32767) ? 32767 : (int)(d0 / THORACIC_DIVISOR);
    thoracic_buf[sample_idx] = clamp;
    clamp = (d1 / ABDOMEN_DIVISOR < -32767) ? -32767 : (d1 / ABDOMEN_DIVISOR > 32767) ? 32767 : (int)(d1 / ABDOMEN_DIVISOR);
    abdomen_buf[sample_idx]  = clamp;

    int fv = (int)(pressure_mbar * 32767.0f / 2.0f);
    flow_buf[sample_idx] = fv < -32767 ? -32767 : fv > 32767 ? 32767 : fv;

    a0x_buf[sample_idx] = a0x;
    a0y_buf[sample_idx] = a0y;
    a0z_buf[sample_idx] = a0z;
    a1x_buf[sample_idx] = a1x;
    a1y_buf[sample_idx] = a1y;
    a1z_buf[sample_idx] = a1z;

    // Captured every 20th 50 Hz tick = 400 ms = 2.5 Hz, matching sigs[SIG_RR].rate above.
    if (sample_idx % 20 == 19 && rr_idx < SAMPLES_RR_HZ)
        rr_buf[rr_idx++] = (int)rr_ms;

    sample_idx++;

    if (sample_idx >= SAMPLES_50HZ) {
        bool ok = write_record_locked();

        sample_idx = 0;
        rr_idx     = 0;
        ecg_idx    = 0;

        if (!ok) {
            stop_on_write_error_locked();
        } else {
            record_count++;

            if (record_count % 10 == 0) {
                // Until this lands, the header still says record_count-10, so a
                // failure here means the last 10 records are on the card but
                // unreadable — worth stopping for, same as a data write failure.
                if (edfflush_file(edf_handle, (int)record_count) != 0) {
                    ESP_LOGE(TAG, "EDF header flush failed at record %lu",
                             (unsigned long)record_count);
                    stop_on_write_error_locked();
                } else {
                    ESP_LOGI(TAG, "EDF: %lu records (%.1f min)",
                             (unsigned long)record_count,
                             (float)record_count * RECORD_DURATION_S / 60.0f);
                    log_flush(); // piggyback on the EDF flush cadence to limit SD wear
                }
            }
        }
    }
    if (edf_mutex) xSemaphoreGive(edf_mutex);
}

void logger_record_ecg(uint16_t ecg_raw)
{
    if (edf_mutex) xSemaphoreTake(edf_mutex, portMAX_DELAY);
    if (!logging_active || edf_handle < 0) {
        if (edf_mutex) xSemaphoreGive(edf_mutex);
        return;
    }
    if (ecg_idx < SAMPLES_100HZ)
        ecg_buf[ecg_idx++] = (int)ecg_raw;
    if (edf_mutex) xSemaphoreGive(edf_mutex);
}

void logger_close(void)
{
    if (edf_mutex) xSemaphoreTake(edf_mutex, portMAX_DELAY);
    if (edf_handle >= 0) {
        edfclose_file(edf_handle);
        edf_handle     = -1;
        logging_active = false;

        struct tm start_t, end_t;
        time_t end_now = time(NULL);
        localtime_r(&recording_start_time, &start_t);
        localtime_r(&end_now, &end_t);
        write_json_sidecar(&start_t, &end_t);
    }
    log_close_file();
    if (edf_mutex) xSemaphoreGive(edf_mutex);
}

bool logger_format_sd(void)
{
    logger_close();

    if (s_card == NULL && !sd_mount()) return false;

    ESP_LOGI(TAG, "Formatting SD card as FAT...");
    esp_err_t r = esp_vfs_fat_sdcard_format("/sdcard", s_card);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "SD format failed: %s", esp_err_to_name(r));
        return false;
    }
    baseline_ok = false;
    ESP_LOGI(TAG, "SD card formatted");
    return true;
}

