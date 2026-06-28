#include "sensors.h"
#include "config.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "sensors";

// ── Globals ───────────────────────────────────────────────────────────────────
volatile int16_t  g_accel0_x, g_accel0_y, g_accel0_z;
volatile int16_t  g_accel1_x, g_accel1_y, g_accel1_z;
volatile uint32_t g_ldc0, g_ldc1;
volatile float    g_pressure_mbar;
volatile uint16_t g_ecg_raw;

// ── I2C handles ───────────────────────────────────────────────────────────────
static i2c_master_bus_handle_t  s_i2c_bus;
static i2c_master_dev_handle_t  s_mma0, s_mma1, s_ldc, s_sdp;
static adc_oneshot_unit_handle_t s_adc;

// ─────────────────────────────────────────────────────────────────────────────
// I2C helpers
// ─────────────────────────────────────────────────────────────────────────────
static esp_err_t i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, 2, 50);
}

static esp_err_t i2c_read_reg(i2c_master_dev_handle_t dev, uint8_t reg,
                               uint8_t *rx, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, rx, len, 50);
}

// ─────────────────────────────────────────────────────────────────────────────
// MMA8451 accelerometer
// ─────────────────────────────────────────────────────────────────────────────
#define MMA_REG_STATUS      0x00
#define MMA_REG_OUT_X_MSB   0x01
#define MMA_REG_WHOAMI      0x0D
#define MMA_REG_XYZ_CFG     0x0E
#define MMA_REG_CTRL1       0x2A
#define MMA_REG_CTRL2       0x2B
#define MMA_REG_PL_CFG      0x11
#define MMA_WHOAMI_VAL      0x1A
// CTRL1: DR=100Hz (bits 5:3 = 010), ACTIVE=1
#define MMA_CTRL1_50HZ_ACTIVE  0x21  // ODR=50Hz (DR=010), ACTIVE bit

static bool mma_init(i2c_master_dev_handle_t dev)
{
    uint8_t id = 0;
    if (i2c_read_reg(dev, MMA_REG_WHOAMI, &id, 1) != ESP_OK || id != MMA_WHOAMI_VAL) {
        return false;
    }
    // Standby → reset
    i2c_write_reg(dev, MMA_REG_CTRL1, 0x00);
    i2c_write_reg(dev, MMA_REG_CTRL2, 0x40);  // RST bit
    vTaskDelay(pdMS_TO_TICKS(2));
    // Config
    i2c_write_reg(dev, MMA_REG_CTRL2,   0x02);  // high-res mode
    i2c_write_reg(dev, MMA_REG_PL_CFG,  0x40);  // portrait detection enable
    i2c_write_reg(dev, MMA_REG_XYZ_CFG, 0x00);  // ±2G range
    i2c_write_reg(dev, MMA_REG_CTRL1,   MMA_CTRL1_50HZ_ACTIVE);
    return true;
}

static void mma_read(i2c_master_dev_handle_t dev,
                     volatile int16_t *ox, volatile int16_t *oy, volatile int16_t *oz)
{
    uint8_t buf[6];
    if (i2c_read_reg(dev, MMA_REG_OUT_X_MSB, buf, 6) != ESP_OK) return;
    *ox = (int16_t)(((uint16_t)buf[0] << 8) | buf[1]) >> 2;
    *oy = (int16_t)(((uint16_t)buf[2] << 8) | buf[3]) >> 2;
    *oz = (int16_t)(((uint16_t)buf[4] << 8) | buf[5]) >> 2;
}

// ─────────────────────────────────────────────────────────────────────────────
// LDC1612 inductive sensor
// ─────────────────────────────────────────────────────────────────────────────
#define LDC_REG_DATA0_MSB    0x00
#define LDC_REG_DATA0_LSB    0x01
#define LDC_REG_DATA1_MSB    0x02
#define LDC_REG_DATA1_LSB    0x03
#define LDC_REG_RCOUNT0      0x08
#define LDC_REG_RCOUNT1      0x09
#define LDC_REG_OFFSET0      0x0C
#define LDC_REG_OFFSET1      0x0D
#define LDC_REG_SETTLECOUNT0 0x10
#define LDC_REG_SETTLECOUNT1 0x11
#define LDC_REG_CLOCK_DIVIDERS0 0x14
#define LDC_REG_CLOCK_DIVIDERS1 0x15
#define LDC_REG_STATUS       0x18
#define LDC_REG_CONFIG       0x1A
#define LDC_REG_MUX_CONFIG   0x1B
#define LDC_REG_RESET_DEV    0x1C
#define LDC_REG_DRIVE0       0x1E
#define LDC_REG_DRIVE1       0x1F
#define LDC_MANUFACTURER_ID  0x5449

static esp_err_t ldc_write16(uint8_t reg, uint16_t val)
{
    uint8_t buf[3] = {reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF)};
    return i2c_master_transmit(s_ldc, buf, 3, 50);
}

static uint16_t ldc_read16(uint8_t reg)
{
    uint8_t rx[2] = {0};
    i2c_read_reg(s_ldc, reg, rx, 2);
    return ((uint16_t)rx[0] << 8) | rx[1];
}

static bool ldc_init(void)
{
    uint16_t mfr = ldc_read16(0x7E);
    if (mfr != LDC_MANUFACTURER_ID) {
        ESP_LOGW(TAG, "LDC1612 not found (mfr=0x%04X)", mfr);
        return false;
    }
    // Reset
    ldc_write16(LDC_REG_RESET_DEV, 0x8000);
    vTaskDelay(pdMS_TO_TICKS(5));

    // Channel config (L=20µH, C=1200pF, Rp=15.727kΩ → Fs≈3.263MHz)
    ldc_write16(LDC_REG_CLOCK_DIVIDERS0, 0x1002);
    ldc_write16(LDC_REG_CLOCK_DIVIDERS1, 0x1002);
    ldc_write16(LDC_REG_SETTLECOUNT0,    0x0400);
    ldc_write16(LDC_REG_SETTLECOUNT1,    0x0400);
    ldc_write16(LDC_REG_RCOUNT0,        0x9000);
    ldc_write16(LDC_REG_RCOUNT1,        0x9000);
    ldc_write16(LDC_REG_OFFSET0,        0x0000);
    ldc_write16(LDC_REG_OFFSET1,        0x0000);
    ldc_write16(LDC_REG_DRIVE0,         0xa000);
    ldc_write16(LDC_REG_DRIVE1,         0xa000);
    ldc_write16(LDC_REG_MUX_CONFIG,     0x820C);
    ldc_write16(LDC_REG_CONFIG,         0x1601);
    return true;
}

static uint32_t ldc_read_channel(uint8_t ch)
{
    uint8_t msb_reg = (ch == 0) ? LDC_REG_DATA0_MSB : LDC_REG_DATA1_MSB;
    uint16_t hi = ldc_read16(msb_reg);
    uint16_t lo = ldc_read16(msb_reg + 1);
    if (hi & 0xF000) return 0;  // error bits set
    return (((uint32_t)(hi & 0x0FFF)) << 16) | lo;
}

// ─────────────────────────────────────────────────────────────────────────────
// SDP800 differential pressure sensor
// ─────────────────────────────────────────────────────────────────────────────
// Continuous measurement, mass flow, no CRC (simplified read)
static bool sdp_init(void)
{
    // Start continuous measurement: 0x3603 (mass flow, no averaging)
    uint8_t cmd[2] = {0x36, 0x03};
    esp_err_t r = i2c_master_transmit(s_sdp, cmd, 2, 50);
    vTaskDelay(pdMS_TO_TICKS(20));
    return r == ESP_OK;
}

static float sdp_read(void)
{
    uint8_t rx[9];
    if (i2c_master_receive(s_sdp, rx, 9, 50) != ESP_OK) return 0.0f;
    int16_t raw   = (int16_t)(((uint16_t)rx[0] << 8) | rx[1]);
    int16_t scale = (int16_t)(((uint16_t)rx[6] << 8) | rx[7]);
    if (scale == 0) return 0.0f;
    return (float)raw / (float)scale;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────
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
    // I2C bus
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port          = I2C_PORT;
    bus_cfg.sda_io_num        = I2C_SDA_PIN;
    bus_cfg.scl_io_num        = I2C_SCL_PIN;
    bus_cfg.clk_source        = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));

    bool ok = true;

    // MMA8451 #0
    if (add_dev(MMA_ADDR_0, &s_mma0) != ESP_OK || !mma_init(s_mma0)) {
        ESP_LOGW(TAG, "MMA8451 #0 not found");
        ok = false;
    }
    // MMA8451 #1
    if (add_dev(MMA_ADDR_1, &s_mma1) != ESP_OK || !mma_init(s_mma1)) {
        ESP_LOGW(TAG, "MMA8451 #1 not found");
        ok = false;
    }
    // LDC1612
    if (add_dev(LDC_ADDR, &s_ldc) != ESP_OK || !ldc_init()) {
        ESP_LOGW(TAG, "LDC1612 not found");
        ok = false;
    }
    // SDP800
    if (add_dev(SDP800_ADDR, &s_sdp) != ESP_OK || !sdp_init()) {
        ESP_LOGW(TAG, "SDP800 not found");
        ok = false;
    }

    // ADC for ECG
    adc_oneshot_unit_init_cfg_t adc_cfg = {};
    adc_cfg.unit_id = ECG_ADC_UNIT;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_cfg, &s_adc));
    adc_oneshot_chan_cfg_t ch_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, ECG_ADC_CHANNEL, &ch_cfg));

    return ok;
}

void sensors_read(void)
{
    mma_read(s_mma0, &g_accel0_x, &g_accel0_y, &g_accel0_z);
    mma_read(s_mma1, &g_accel1_x, &g_accel1_y, &g_accel1_z);
    g_ldc0        = ldc_read_channel(0);
    g_ldc1        = ldc_read_channel(1);
    g_pressure_mbar = sdp_read();
}

void sensors_read_ecg(void)
{
    int raw = 0;
    adc_oneshot_read(s_adc, ECG_ADC_CHANNEL, &raw);
    g_ecg_raw = (uint16_t)raw;
}
