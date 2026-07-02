#include "DS1307.h"

#define DS1307_REG_SECONDS  0x00
#define DS1307_CH_BIT       0x80  // seconds register bit 7: clock halt

static esp_err_t i2c_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *rx, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, rx, len, 50);
}

static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v & 0x0F) + ((v >> 4) * 10)); }
static uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

bool ds1307_init(i2c_master_dev_handle_t dev)
{
    uint8_t seconds = 0;
    return i2c_read_reg(dev, DS1307_REG_SECONDS, &seconds, 1) == ESP_OK;
}

bool ds1307_get_time(i2c_master_dev_handle_t dev, struct tm *out)
{
    uint8_t buf[7];
    if (i2c_read_reg(dev, DS1307_REG_SECONDS, buf, sizeof(buf)) != ESP_OK)
        return false;

    if (buf[0] & DS1307_CH_BIT)
        return false;  // oscillator halted - RTC has never been set

    out->tm_sec  = bcd2bin(buf[0] & 0x7F);
    out->tm_min  = bcd2bin(buf[1] & 0x7F);
    out->tm_hour = bcd2bin(buf[2] & 0x3F);      // assumes 24-hour mode (bit6=0)
    out->tm_wday = bcd2bin(buf[3] & 0x07) - 1;  // DS1307: 1-7 -> struct tm: 0-6
    out->tm_mday = bcd2bin(buf[4] & 0x3F);
    out->tm_mon  = bcd2bin(buf[5] & 0x1F) - 1;  // DS1307: 1-12 -> struct tm: 0-11
    out->tm_year = bcd2bin(buf[6]) + 100;       // DS1307: 00-99 -> struct tm: years since 1900
    out->tm_isdst = 0;
    return true;
}

bool ds1307_set_time(i2c_master_dev_handle_t dev, const struct tm *t)
{
    uint8_t buf[8];
    buf[0] = DS1307_REG_SECONDS;
    buf[1] = bin2bcd((uint8_t)t->tm_sec);              // bit7=0 also starts the oscillator
    buf[2] = bin2bcd((uint8_t)t->tm_min);
    buf[3] = bin2bcd((uint8_t)t->tm_hour);             // 24-hour mode (bit6=0)
    buf[4] = bin2bcd((uint8_t)((t->tm_wday % 7) + 1));
    buf[5] = bin2bcd((uint8_t)t->tm_mday);
    buf[6] = bin2bcd((uint8_t)(t->tm_mon + 1));
    buf[7] = bin2bcd((uint8_t)(t->tm_year % 100));
    return i2c_master_transmit(dev, buf, sizeof(buf), 50) == ESP_OK;
}
