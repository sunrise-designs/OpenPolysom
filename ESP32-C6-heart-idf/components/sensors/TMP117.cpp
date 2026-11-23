#include "TMP117.h"
#include "esp_log.h"

static const char *TAG = "TMP117";

// Register map (TMP117 datasheet §7.6). All registers are 16 bits, MSB first.
#define TMP117_REG_TEMP_RESULT  0x00
#define TMP117_REG_CONFIG       0x01
#define TMP117_REG_DEVICE_ID    0x0F

// Device_ID bits 11:0 are the device ID; bits 15:12 are a revision number,
// which is masked off before comparing.
#define TMP117_DEVICE_ID        0x0117
#define TMP117_DEVICE_ID_MASK   0x0FFF

// Temp_Result reads 0x8000 (-256 °C) after power-up until the first
// conversion has finished (datasheet §7.6.1.1).
#define TMP117_TEMP_NOT_READY   ((int16_t)0x8000)

// One LSB of Temp_Result is 7.8125 m°C = 1/128 °C.
#define TMP117_DEG_C_PER_LSB    0.0078125f

// Configuration register value written at init:
//   MOD  [11:10] = 00  continuous conversion
//   CONV [9:7]   = 100 1 s conversion cycle
//   AVG  [6:5]   = 01  8 averages (125 ms active, then standby)
// This is the factory power-up default, written explicitly in case the
// sensor's EEPROM was reprogrammed. The 1 s cycle keeps self-heating low; for
// body/skin temperature that rate is plenty. sensors_read() polls faster than
// this and simply gets the latest result again. Writing Configuration
// does not touch EEPROM, so this does not wear it.
#define TMP117_CONFIG_VALUE     0x0220

static bool tmp117_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t *out)
{
    uint8_t rx[2];
    if (i2c_master_transmit_receive(dev, &reg, 1, rx, 2, 50) != ESP_OK)
        return false;
    *out = ((uint16_t)rx[0] << 8) | rx[1];
    return true;
}

static bool tmp117_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t value)
{
    uint8_t tx[3] = {reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
    return i2c_master_transmit(dev, tx, 3, 50) == ESP_OK;
}

bool tmp117_init(tmp117_t *s, i2c_master_dev_handle_t dev, uint8_t addr)
{
    s->dev          = dev;
    s->addr         = addr;
    s->read_failing = false;

    uint16_t id;
    if (!tmp117_read_reg(dev, TMP117_REG_DEVICE_ID, &id)) {
        ESP_LOGW(TAG, "0x%02X: failed to read device ID", addr);
        return false;
    }
    if ((id & TMP117_DEVICE_ID_MASK) != TMP117_DEVICE_ID) {
        ESP_LOGW(TAG, "0x%02X: device ID 0x%04X - NOT a TMP117", addr, id);
        return false;
    }
    ESP_LOGI(TAG, "0x%02X: confirmed TMP117 (rev %u)", addr, id >> 12);

    if (!tmp117_write_reg(dev, TMP117_REG_CONFIG, TMP117_CONFIG_VALUE)) {
        ESP_LOGW(TAG, "0x%02X: failed to write configuration", addr);
        return false;
    }
    return true;
}

// Logs a failure transition once, not every call, since this runs at
// SENSOR_RATE_HZ and would otherwise flood the log while the sensor stays down.
bool tmp117_read(tmp117_t *s, float *out_c)
{
    uint16_t raw;
    if (!tmp117_read_reg(s->dev, TMP117_REG_TEMP_RESULT, &raw)) {
        if (!s->read_failing) {
            ESP_LOGW(TAG, "0x%02X: read failed", s->addr);
            s->read_failing = true;
        }
        return false;
    }

    // Not a fault: the first conversion after power-up just hasn't finished.
    if ((int16_t)raw == TMP117_TEMP_NOT_READY)
        return false;

    if (s->read_failing) {
        ESP_LOGI(TAG, "0x%02X: read recovered", s->addr);
        s->read_failing = false;
    }
    *out_c = (float)(int16_t)raw * TMP117_DEG_C_PER_LSB;
    return true;
}
