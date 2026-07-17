#include "DS3231.h"

#define DS3231_REG_SECONDS  0x00
#define DS3231_REG_STATUS   0x0F
#define DS3231_STATUS_OSF   0x80  // control/status register bit 7: oscillator stop flag

static esp_err_t i2c_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *rx, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, rx, len, 50);
}

static esp_err_t i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, sizeof(buf), 50);
}

static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v & 0x0F) + ((v >> 4) * 10)); }
static uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

bool ds3231_init(i2c_master_dev_handle_t dev)
{
    uint8_t seconds = 0;
    return i2c_read_reg(dev, DS3231_REG_SECONDS, &seconds, 1) == ESP_OK;
}

bool ds3231_get_time(i2c_master_dev_handle_t dev, struct tm *out)
{
    // Unlike the DS1307's CH bit in the seconds register, the DS3231 has its
    // own integrated oscillator and flags "time may be wrong" via OSF in the
    // control/status register instead.
    uint8_t status = 0;
    if (i2c_read_reg(dev, DS3231_REG_STATUS, &status, 1) != ESP_OK)
        return false;
    if (status & DS3231_STATUS_OSF)
        return false;  // oscillator stopped at some point - RTC has never been set, or lost power

    uint8_t buf[7];
    if (i2c_read_reg(dev, DS3231_REG_SECONDS, buf, sizeof(buf)) != ESP_OK)
        return false;

    out->tm_sec  = bcd2bin(buf[0] & 0x7F);
    out->tm_min  = bcd2bin(buf[1] & 0x7F);
    out->tm_hour = bcd2bin(buf[2] & 0x3F);      // assumes 24-hour mode (bit6=0)
    out->tm_wday = bcd2bin(buf[3] & 0x07) - 1;  // DS3231: 1-7 -> struct tm: 0-6
    out->tm_mday = bcd2bin(buf[4] & 0x3F);
    out->tm_mon  = bcd2bin(buf[5] & 0x1F) - 1;  // DS3231: 1-12 -> struct tm: 0-11 (mask drops the century bit)
    out->tm_year = bcd2bin(buf[6]) + 100;       // DS3231: 00-99 -> struct tm: years since 1900
    out->tm_isdst = 0;
    return true;
}

bool ds3231_set_time(i2c_master_dev_handle_t dev, const struct tm *t)
{
    uint8_t buf[8];
    buf[0] = DS3231_REG_SECONDS;
    buf[1] = bin2bcd((uint8_t)t->tm_sec);
    buf[2] = bin2bcd((uint8_t)t->tm_min);
    buf[3] = bin2bcd((uint8_t)t->tm_hour);             // 24-hour mode (bit6=0)
    buf[4] = bin2bcd((uint8_t)((t->tm_wday % 7) + 1));
    buf[5] = bin2bcd((uint8_t)t->tm_mday);
    buf[6] = bin2bcd((uint8_t)(t->tm_mon + 1));
    buf[7] = bin2bcd((uint8_t)(t->tm_year % 100));
    if (i2c_master_transmit(dev, buf, sizeof(buf), 50) != ESP_OK)
        return false;

    // The DS3231 doesn't clear OSF on its own when the clock is (re)set —
    // clear it explicitly so a subsequent ds3231_get_time() trusts the time.
    uint8_t status = 0;
    if (i2c_read_reg(dev, DS3231_REG_STATUS, &status, 1) != ESP_OK)
        return false;
    return i2c_write_reg(dev, DS3231_REG_STATUS, status & (uint8_t)~DS3231_STATUS_OSF) == ESP_OK;
}
