#include "LDC1612.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "LDC1612";

#define LDC_REG_DATA0_MSB       0x00
#define LDC_REG_DATA1_MSB       0x02
#define LDC_REG_RCOUNT0         0x08
#define LDC_REG_RCOUNT1         0x09
#define LDC_REG_OFFSET0         0x0C
#define LDC_REG_OFFSET1         0x0D
#define LDC_REG_SETTLECOUNT0    0x10
#define LDC_REG_SETTLECOUNT1    0x11
#define LDC_REG_CLOCK_DIVIDERS0 0x14
#define LDC_REG_CLOCK_DIVIDERS1 0x15
#define LDC_REG_CONFIG          0x1A
#define LDC_REG_MUX_CONFIG      0x1B
#define LDC_REG_RESET_DEV       0x1C
#define LDC_REG_DRIVE0          0x1E
#define LDC_REG_DRIVE1          0x1F
#define LDC_MANUFACTURER_ID     0x5449

static esp_err_t i2c_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *rx, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, rx, len, 50);
}

static esp_err_t ldc_write16(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t val)
{
    uint8_t buf[3] = {reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF)};
    return i2c_master_transmit(dev, buf, 3, 50);
}

static uint16_t ldc_read16(i2c_master_dev_handle_t dev, uint8_t reg)
{
    uint8_t rx[2] = {0};
    i2c_read_reg(dev, reg, rx, 2);
    return ((uint16_t)rx[0] << 8) | rx[1];
}

bool ldc1612_init(i2c_master_dev_handle_t dev)
{
    uint16_t mfr = ldc_read16(dev, 0x7E);
    if (mfr != LDC_MANUFACTURER_ID) {
        ESP_LOGW(TAG, "LDC1612 not found (mfr=0x%04X)", mfr);
        return false;
    }
    ldc_write16(dev, LDC_REG_RESET_DEV, 0x8000);
    vTaskDelay(pdMS_TO_TICKS(5));

    // Channel config (L=20µH, C=1200pF, Rp=15.727kΩ → Fs≈3.263MHz)
    ldc_write16(dev, LDC_REG_CLOCK_DIVIDERS0, 0x1002);
    ldc_write16(dev, LDC_REG_CLOCK_DIVIDERS1, 0x1002);
    ldc_write16(dev, LDC_REG_SETTLECOUNT0,    0x0400);
    ldc_write16(dev, LDC_REG_SETTLECOUNT1,    0x0400);
    ldc_write16(dev, LDC_REG_RCOUNT0,         0x9000);
    ldc_write16(dev, LDC_REG_RCOUNT1,         0x9000);
    ldc_write16(dev, LDC_REG_OFFSET0,         0x0000);
    ldc_write16(dev, LDC_REG_OFFSET1,         0x0000);
    ldc_write16(dev, LDC_REG_DRIVE0,          0xa000);
    ldc_write16(dev, LDC_REG_DRIVE1,          0xa000);
    ldc_write16(dev, LDC_REG_MUX_CONFIG,      0x820C);
    ldc_write16(dev, LDC_REG_CONFIG,          0x1601);
    return true;
}

uint32_t ldc1612_read_channel(i2c_master_dev_handle_t dev, uint8_t ch)
{
    uint8_t msb_reg = (ch == 0) ? LDC_REG_DATA0_MSB : LDC_REG_DATA1_MSB;
    uint16_t hi = ldc_read16(dev, msb_reg);
    uint16_t lo = ldc_read16(dev, msb_reg + 1);
    if (hi & 0xF000) return 0;  // error bits set
    return (((uint32_t)(hi & 0x0FFF)) << 16) | lo;
}
