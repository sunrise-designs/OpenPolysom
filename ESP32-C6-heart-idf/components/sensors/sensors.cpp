#include "sensors.h"
#include "MMA8451.h"
#include "LDC1612.h"
#include "SDP800.h"
#include "DS1307.h"
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
static i2c_master_dev_handle_t   s_mma0, s_mma1, s_ldc, s_sdp, s_ds1307;
static adc_oneshot_unit_handle_t s_adc;

static esp_err_t add_dev(uint8_t addr, i2c_master_dev_handle_t *out)
{
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address  = addr;
    cfg.scl_speed_hz    = I2C_FREQ_HZ;
    return i2c_master_bus_add_device(s_i2c_bus, &cfg, out);
}

bool sensors_init(void)
{
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port          = I2C_PORT;
    bus_cfg.sda_io_num        = I2C_SDA_PIN;
    bus_cfg.scl_io_num        = I2C_SCL_PIN;
    bus_cfg.clk_source        = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));

    bool ok = true;

    g_mma0_present = (add_dev(MMA_ADDR_0, &s_mma0) == ESP_OK) && mma8451_init(s_mma0);
    if (!g_mma0_present) {
        ESP_LOGW(TAG, "MMA8451 #0 not found");
        ok = false;
    }
    g_mma1_present = (add_dev(MMA_ADDR_1, &s_mma1) == ESP_OK) && mma8451_init(s_mma1);
    if (!g_mma1_present) {
        ESP_LOGW(TAG, "MMA8451 #1 not found");
        ok = false;
    }
    g_ldc_present = (add_dev(LDC_ADDR, &s_ldc) == ESP_OK) && ldc1612_init(s_ldc);
    if (!g_ldc_present) {
        ESP_LOGW(TAG, "LDC1612 not found");
        ok = false;
    }
    g_sdp_present = (add_dev(SDP800_ADDR, &s_sdp) == ESP_OK) && sdp800_init(s_sdp);
    if (!g_sdp_present) {
        ESP_LOGW(TAG, "SDP800 not found");
        ok = false;
    }

    // The RTC is optional (not fitted on every board), so its absence does
    // not fail sensors_init() the way the other sensors do.
    g_rtc_present = (add_dev(DS1307_ADDR, &s_ds1307) == ESP_OK) && ds1307_init(s_ds1307);
    if (!g_rtc_present) {
        ESP_LOGW(TAG, "DS1307 RTC not found");
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
    // EDF+ Flow channel's declared physical dimension.
    g_pressure_mbar = sdp800_read(s_sdp) / 100.0f;
}

void sensors_read_ecg(void)
{
    int raw = 0;
    adc_oneshot_read(s_adc, (adc_channel_t)ECG_ADC_CHANNEL, &raw);
    g_ecg_raw = (uint16_t)raw;
}

// ── RTC (DS1307) time sync ──────────────────────────────────────────────────

// Reads the RTC and treats it as valid only once it holds a plausible time —
// i.e. it has already been set at least once (year >= 2026 rules out the
// power-up default of the DS1307's clock never having been set, or a dead
// battery having reset it to an earlier date).
static bool read_valid_rtc_time(struct tm *out)
{
    if (!g_rtc_present) return false;
    if (!ds1307_get_time(s_ds1307, out)) return false;
    return (out->tm_year + 1900) >= 2026;
}

// t is interpreted as UTC (this project always stores UTC in the RTC, same
// as what SNTP puts in the system clock). Temporarily switches TZ to UTC so
// mktime() doesn't apply the local offset, then restores the project's TZ.
static void apply_utc_tm_to_system_clock(const struct tm *t)
{
    struct tm tmp = *t;
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t utc = mktime(&tmp);
    setenv("TZ", LOCAL_TZ, 1);
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
    if (!ds1307_set_time(s_ds1307, &t)) {
        ESP_LOGW(TAG, "Failed to write time to RTC");
    }
}

void sensors_rtc_periodic_sync(void)
{
    struct tm t;
    if (!read_valid_rtc_time(&t)) return;
    apply_utc_tm_to_system_clock(&t);
}
