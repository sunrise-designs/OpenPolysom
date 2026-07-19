#include "sensors.h"
#include "MMA8451.h"
#include "LDC1612.h"
#include "SDP800.h"
#include "DS3231.h"
#include "config.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "sensors";

// ── Globals ───────────────────────────────────────────────────────────────────
volatile int16_t  g_accel0_x, g_accel0_y, g_accel0_z;
volatile int16_t  g_accel1_x, g_accel1_y, g_accel1_z;
volatile uint32_t g_ldc0, g_ldc1;
volatile float    g_pressure_mbar;
volatile uint16_t g_ecg_raw;
volatile bool     g_rtc_present;
volatile bool     g_mma0_present;
volatile bool     g_mma1_present;
volatile bool     g_ldc_present;
volatile bool     g_sdp_present;

// ── I2C handles ───────────────────────────────────────────────────────────────
static i2c_master_bus_handle_t   s_i2c_bus;
static i2c_master_dev_handle_t   s_mma0, s_mma1, s_ldc, s_sdp, s_ds3231;
static adc_oneshot_unit_handle_t s_adc;

adc_oneshot_unit_handle_t sensors_get_adc_unit(void)
{
    return s_adc;
}

static esp_err_t add_dev(uint8_t addr, i2c_master_dev_handle_t *out)
{
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address  = addr;
    cfg.scl_speed_hz    = I2C_FREQ_HZ;
    return i2c_master_bus_add_device(s_i2c_bus, &cfg, out);
}

bool sensors_init(i2c_master_bus_handle_t bus)
{
    vTaskDelay(pdMS_TO_TICKS(50));  // wait for sensors to power up (SDP800 datasheet §3.2 says 25ms minimum)
    s_i2c_bus = bus;

    bool ok = true;

    g_mma0_present = (add_dev(MMA_ADDR_0, &s_mma0) == ESP_OK) && mma8451_init(s_mma0);
    if (!g_mma0_present) {
        ESP_LOGW(TAG, "MMA8451 #0 not found");
        ok = false;
    }
    else {
        ESP_LOGI(TAG, "MMA8451 #0 detected");
    }
    g_mma1_present = (add_dev(MMA_ADDR_1, &s_mma1) == ESP_OK) && mma8451_init(s_mma1);
    if (!g_mma1_present) {
        ESP_LOGW(TAG, "MMA8451 #1 not found");
        ok = false;
    }
    else {
        ESP_LOGI(TAG, "MMA8451 #1 detected");
    }
    g_ldc_present = (add_dev(LDC_ADDR, &s_ldc) == ESP_OK) && ldc1612_init(s_ldc);
    if (!g_ldc_present) {
        ESP_LOGW(TAG, "LDC1612 not found");
        ok = false;
    }
    else {
        ESP_LOGI(TAG, "LDC1612 detected");
    }
    g_sdp_present = (add_dev(SDP800_ADDR, &s_sdp) == ESP_OK);
    if (!g_sdp_present) {
        ESP_LOGW(TAG, "SDP800 device not found on address 0x%02X", SDP800_ADDR);
        ok = false;
    }
    else {
        g_sdp_present = sdp800_init(s_sdp);
        if (!g_sdp_present) {
            ESP_LOGW(TAG, "SDP800 detected but failed to initialize");
            ok = false;
        }
        else {
            ESP_LOGI(TAG, "SDP800 detected");
        }
    }


    // The RTC is optional (not fitted on every board), so its absence does
    // not fail sensors_init() the way the other sensors do.
    g_rtc_present = (add_dev(DS3231_ADDR, &s_ds3231) == ESP_OK);
    if (!g_rtc_present) {
        ESP_LOGW(TAG, "DS3231 RTC device not found on 0x%02X", DS3231_ADDR);
    }
    else {
        if (ds3231_init(s_ds3231)) {
            ESP_LOGI(TAG, "DS3231 RTC detected");
        }
        else {
            ESP_LOGW(TAG, "DS3231 RTC detected but failed to initialize");
            g_rtc_present = false;
        }
    }

    adc_oneshot_unit_init_cfg_t adc_cfg = {};
    adc_cfg.unit_id = ECG_ADC_UNIT;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_cfg, &s_adc));
    adc_oneshot_chan_cfg_t ch_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, (adc_channel_t)ECG_ADC_CHANNEL, &ch_cfg));

    return ok;
}

void sensors_read(void)
{
    mma8451_read(s_mma0, &g_accel0_x, &g_accel0_y, &g_accel0_z);
    mma8451_read(s_mma1, &g_accel1_x, &g_accel1_y, &g_accel1_z);
    g_ldc0          = ldc1612_read_channel(s_ldc, 0);
    g_ldc1          = ldc1612_read_channel(s_ldc, 1);
    // sdp800_read() returns Pascals (raw / on-chip scale factor, §6.5 of the
    // SDP810-125Pa datasheet); convert to mbar (1 mbar = 100 Pa) to match the
    // EDF+ Flow channel's declared physical dimension. Skip the read entirely
    // when init failed, rather than polling a sensor that was never told to
    // start continuous measurement.
    g_pressure_mbar = g_sdp_present ? sdp800_read(s_sdp) : 0.0f;
}

void sensors_read_ecg(void)
{
    int raw = 0;
    adc_oneshot_read(s_adc, (adc_channel_t)ECG_ADC_CHANNEL, &raw);
    g_ecg_raw = (uint16_t)raw;
}

// ── RTC (DS3231) time sync ──────────────────────────────────────────────────

// Reads the RTC and treats it as valid only once it holds a plausible time —
// i.e. it has already been set at least once (year >= 2026 rules out the
// power-up default of the DS3231's clock never having been set, or a dead
// battery having reset it to an earlier date).
static bool read_valid_rtc_time(struct tm *out)
{
    if (!g_rtc_present) return false;
    if (!ds3231_get_time(s_ds3231, out)) return false;
    return (out->tm_year + 1900) >= 2026;
}

// t is interpreted as UTC (this project always stores UTC in the RTC, same
// as what SNTP puts in the system clock). Temporarily switches TZ to UTC so
// mktime() doesn't apply the local offset, then restores the project's TZ.
static void apply_utc_tm_to_system_clock(const struct tm *t)
{
    struct tm tmp = *t;
    const char *current_tz = getenv("TZ");
    char saved_tz[64] = {0};
    if (current_tz) {
        strncpy(saved_tz, current_tz, sizeof(saved_tz) - 1);
    }

    setenv("TZ", "UTC0", 1);
    tzset();
    time_t utc = mktime(&tmp);

    if (current_tz && saved_tz[0] != '\0') {
        setenv("TZ", saved_tz, 1);
    } else {
        setenv("TZ", LOCAL_TZ, 1);
    }
    tzset();

    struct timeval tv = { .tv_sec = utc, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

bool sensors_rtc_startup_sync(void)
{
    struct tm t;
    if (!read_valid_rtc_time(&t)) return false;

    apply_utc_tm_to_system_clock(&t);
    ESP_LOGI(TAG, "System clock set from RTC: %04d-%02d-%02d %02d:%02d:%02d UTC",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return true;
}

void sensors_rtc_write_from_system(void)
{
    if (!g_rtc_present) return;

    time_t now = time(NULL);
    struct tm t;
    gmtime_r(&now, &t);
    if (!ds3231_set_time(s_ds3231, &t)) {
        ESP_LOGW(TAG, "Failed to write time to RTC");
    }
}

void sensors_rtc_periodic_sync(void)
{
    struct tm t;
    if (!read_valid_rtc_time(&t)) return;
    apply_utc_tm_to_system_clock(&t);
}
