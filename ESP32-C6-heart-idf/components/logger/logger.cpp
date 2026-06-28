#include "logger.h"
#include "config.h"
#include "edflib.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/spi_master.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include <time.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "logger";

volatile bool g_dumping = false;

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

static int edf_handle = -1;
static bool logging_active = false;
static uint32_t record_count = 0;

// ── Sample buffers ────────────────────────────────────────────────────────────
static int thoracic_buf[SAMPLES_50HZ];
static int abdomen_buf[SAMPLES_50HZ];
static int flow_buf[SAMPLES_50HZ];
static int ecg_buf[SAMPLES_50HZ];
static int a0x_buf[SAMPLES_50HZ], a0y_buf[SAMPLES_50HZ], a0z_buf[SAMPLES_50HZ];
static int a1x_buf[SAMPLES_50HZ], a1y_buf[SAMPLES_50HZ], a1z_buf[SAMPLES_50HZ];
static int rr_buf[SAMPLES_1HZ];

static int sample_idx = 0;
static int rr_idx     = 0;

// ── LDC baseline ──────────────────────────────────────────────────────────────
static uint32_t ldc0_baseline   = 0;
static uint32_t ldc1_baseline   = 0;
static bool     baseline_ok     = false;

uint32_t logger_get_ldc0_baseline(void) { return ldc0_baseline; }
uint32_t logger_get_ldc1_baseline(void) { return ldc1_baseline; }
bool     logger_get_baseline_ok(void)   { return baseline_ok; }

// ── SD card ───────────────────────────────────────────────────────────────────
static sdmmc_card_t *s_card = NULL;

static bool sd_mount(void)
{
    // SPI bus must already be initialised by display_init().
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

// ── EDF setup ────────────────────────────────────────────────────────────────
static bool open_edf(void)
{
    edf_handle = edfopen_file_writeonly(EDF_FILE_PATH,
                                        EDFLIB_FILETYPE_EDFPLUS, NUM_SIGNALS);
    if (edf_handle < 0) {
        ESP_LOGE(TAG, "edfopen failed: %d", edf_handle);
        return false;
    }

    // 10-second data records (1 000 000 × 10 µs)
    edf_set_datarecord_duration(edf_handle, 1000000);

    typedef struct { const char *label, *transducer, *dim; int rate, dmax, dmin;
                     double pmax, pmin; } SigDef;
    static const SigDef sigs[NUM_SIGNALS] = {
        {"Thoracic","LDC1612 CH0",  "counts",50, 32767,-32767,  1e6, -1e6},
        {"Abdomen", "LDC1612 CH1",  "counts",50, 32767,-32767,  1e6, -1e6},
        {"Flow",    "SDP800-125Pa", "mbar",  50, 32767,-32767,  2.0, -2.0},
        {"ECG",     "AD8232 ADC",   "ADC",   50,  4095,     0, 4095.0, 0.0},
        {"Accel0X", "MMA8451 ch0",  "mg",    50,  8191, -8192, 2000.0,-2000.0},
        {"Accel0Y", "MMA8451 ch0",  "mg",    50,  8191, -8192, 2000.0,-2000.0},
        {"Accel0Z", "MMA8451 ch0",  "mg",    50,  8191, -8192, 2000.0,-2000.0},
        {"Accel1X", "MMA8451 ch1",  "mg",    50,  8191, -8192, 2000.0,-2000.0},
        {"Accel1Y", "MMA8451 ch1",  "mg",    50,  8191, -8192, 2000.0,-2000.0},
        {"Accel1Z", "MMA8451 ch1",  "mg",    50,  8191, -8192, 2000.0,-2000.0},
        {"RR",      "Polar H9 BLE", "ms",     1,  2000,     0, 2000.0, 0.0},
    };

    for (int i = 0; i < NUM_SIGNALS; i++) {
        edf_set_label(edf_handle, i, sigs[i].label);
        edf_set_transducer(edf_handle, i, sigs[i].transducer);
        edf_set_samplefrequency(edf_handle, i, sigs[i].rate);
        edf_set_digital_maximum(edf_handle, i, sigs[i].dmax);
        edf_set_digital_minimum(edf_handle, i, sigs[i].dmin);
        edf_set_physical_maximum(edf_handle, i, sigs[i].pmax);
        edf_set_physical_minimum(edf_handle, i, sigs[i].pmin);
        edf_set_physical_dimension(edf_handle, i, sigs[i].dim);
    }

    struct tm t = {};
    time_t now = time(NULL);
    localtime_r(&now, &t);
    edf_set_startdatetime(edf_handle,
                          t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                          t.tm_hour, t.tm_min, t.tm_sec);

    sample_idx    = 0;
    rr_idx        = 0;
    record_count  = 0;
    logging_active = true;
    ESP_LOGI(TAG, "EDF recording started: %s", EDF_FILE_PATH);
    return true;
}

bool logger_init(void)
{
    if (!sd_mount()) return false;
    return open_edf();
}

void logger_record(int16_t  a0x, int16_t  a0y, int16_t  a0z,
                   int16_t  a1x, int16_t  a1y, int16_t  a1z,
                   uint32_t ldc0, uint32_t ldc1,
                   uint16_t ecg,  float    pressure_mbar,
                   uint16_t rr_ms)
{
    if (!logging_active || edf_handle < 0) return;

    if (!baseline_ok && ldc0 != 0) {
        ldc0_baseline = ldc0;
        ldc1_baseline = ldc1;
        baseline_ok   = true;
    }

    int32_t d0 = (int32_t)(ldc0 - ldc0_baseline);
    int32_t d1 = (int32_t)(ldc1 - ldc1_baseline);

    int clamp = (d0 / 30 < -32767) ? -32767 : (d0 / 30 > 32767) ? 32767 : (int)(d0 / 30);
    thoracic_buf[sample_idx] = clamp;
    clamp = (d1 / 30 < -32767) ? -32767 : (d1 / 30 > 32767) ? 32767 : (int)(d1 / 30);
    abdomen_buf[sample_idx]  = clamp;

    int fv = (int)(pressure_mbar * 32767.0f / 2.0f);
    flow_buf[sample_idx] = fv < -32767 ? -32767 : fv > 32767 ? 32767 : fv;
    ecg_buf[sample_idx]  = (int)ecg;

    a0x_buf[sample_idx] = a0x;
    a0y_buf[sample_idx] = a0y;
    a0z_buf[sample_idx] = a0z;
    a1x_buf[sample_idx] = a1x;
    a1y_buf[sample_idx] = a1y;
    a1z_buf[sample_idx] = a1z;

    if (sample_idx % 50 == 49 && rr_idx < SAMPLES_1HZ)
        rr_buf[rr_idx++] = (int)rr_ms;

    sample_idx++;

    if (sample_idx >= SAMPLES_50HZ) {
        edfwrite_digital_samples(edf_handle, thoracic_buf);
        edfwrite_digital_samples(edf_handle, abdomen_buf);
        edfwrite_digital_samples(edf_handle, flow_buf);
        edfwrite_digital_samples(edf_handle, ecg_buf);
        edfwrite_digital_samples(edf_handle, a0x_buf);
        edfwrite_digital_samples(edf_handle, a0y_buf);
        edfwrite_digital_samples(edf_handle, a0z_buf);
        edfwrite_digital_samples(edf_handle, a1x_buf);
        edfwrite_digital_samples(edf_handle, a1y_buf);
        edfwrite_digital_samples(edf_handle, a1z_buf);
        edfwrite_digital_samples(edf_handle, rr_buf);

        sample_idx = 0;
        rr_idx     = 0;
        record_count++;

        if (record_count % 10 == 0) {
            edfflush_file(edf_handle, (int)record_count);
            ESP_LOGI(TAG, "EDF: %lu records (%.1f min)",
                     (unsigned long)record_count,
                     (float)record_count * RECORD_DURATION_S / 60.0f);
        }
    }
}

void logger_process_cmd(char cmd)
{
    if (cmd == 'E') {
        logger_close();
        remove(EDF_FILE_PATH);
        baseline_ok = false;
        open_edf();
    }
}

void logger_close(void)
{
    if (edf_handle >= 0) {
        edfclose_file(edf_handle);
        edf_handle     = -1;
        logging_active = false;
    }
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
