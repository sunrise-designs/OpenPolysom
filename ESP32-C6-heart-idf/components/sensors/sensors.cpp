#include "sensors.h"
#include "MMA8451.h"
#include "LDC1612.h"
#include "SDP800.h"
#include "config.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sensors";

// ── Globals ───────────────────────────────────────────────────────────────────
volatile int16_t  g_accel0_x, g_accel0_y, g_accel0_z;
volatile int16_t  g_accel1_x, g_accel1_y, g_accel1_z;
volatile uint32_t g_ldc0, g_ldc1;
volatile float    g_pressure_mbar;
volatile uint16_t g_ecg_raw;

// ── I2C handles ───────────────────────────────────────────────────────────────
static i2c_master_bus_handle_t   s_i2c_bus;
static i2c_master_dev_handle_t   s_mma0, s_mma1, s_ldc, s_sdp;
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

    if (add_dev(MMA_ADDR_0, &s_mma0) != ESP_OK || !mma8451_init(s_mma0)) {
        ESP_LOGW(TAG, "MMA8451 #0 not found");
        ok = false;
    }
    if (add_dev(MMA_ADDR_1, &s_mma1) != ESP_OK || !mma8451_init(s_mma1)) {
        ESP_LOGW(TAG, "MMA8451 #1 not found");
        ok = false;
    }
    if (add_dev(LDC_ADDR, &s_ldc) != ESP_OK || !ldc1612_init(s_ldc)) {
        ESP_LOGW(TAG, "LDC1612 not found");
        ok = false;
    }
    if (add_dev(SDP800_ADDR, &s_sdp) != ESP_OK || !sdp800_init(s_sdp)) {
        ESP_LOGW(TAG, "SDP800 not found");
        ok = false;
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
    g_pressure_mbar = sdp800_read(s_sdp);
}

void sensors_read_ecg(void)
{
    int raw = 0;
    adc_oneshot_read(s_adc, (adc_channel_t)ECG_ADC_CHANNEL, &raw);
    g_ecg_raw = (uint16_t)raw;
}
