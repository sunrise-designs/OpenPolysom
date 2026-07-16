#include "SDP800.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <stddef.h>

static const char *TAG = "SDP800";

// Sensirion CRC-8: polynomial 0x31 (x^8 + x^5 + x^4 + 1), initialization 0xFF.
static uint8_t sdp800_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    return crc;
}

// "Read product identifier" sequence (SDP8xx datasheet §6.3.6, mirrors
// Sensirion's arduino-i2c-sdp readProductIdentifier()): two back-to-back
// write commands, then an 18-byte read — six (2 data bytes + 1 CRC) groups,
// the first two forming the product number, the last four the serial number.
// Per §6.3.2 the sensor must still be idle to accept this — it must run
// before continuous measurement is started, not after.
bool sdp800_read_product_id(i2c_master_dev_handle_t dev, sdp800_id_t *out)
{
    uint8_t cmd1[2] = {0x36, 0x7C};
    if (i2c_master_transmit(dev, cmd1, 2, 50) != ESP_OK)
        return false;

    uint8_t cmd2[2] = {0xE1, 0x02};
    if (i2c_master_transmit(dev, cmd2, 2, 50) != ESP_OK)
        return false;

    uint8_t rx[18];
    if (i2c_master_receive(dev, rx, 18, 50) != ESP_OK)
        return false;

    uint32_t product_number = 0;
    uint64_t serial_number  = 0;

    for (int group = 0; group < 6; group++) {
        const uint8_t *w = &rx[group * 3];
        if (sdp800_crc8(w, 2) != w[2])
            return false;

        uint16_t word = ((uint16_t)w[0] << 8) | w[1];
        if (group < 2)
            product_number = (product_number << 16) | word;
        else
            serial_number = (serial_number << 16) | word;
    }

    out->product_number = product_number;
    out->serial_number  = serial_number;
    return true;
}

bool sdp800_init(i2c_master_dev_handle_t dev)
{
    // Must run first: once continuous measurement starts, the sensor won't
    // accept another command until it's stopped (datasheet §6.3.2).
    sdp800_id_t id;
    bool got_id = sdp800_read_product_id(dev, &id);
    bool is_sdp810_125pa = got_id &&
        (id.product_number & SDP8XX_PRODUCT_NUMBER_VARIANT_MASK) == SDP810_125PA_PRODUCT_NUMBER;

    if (got_id) {
        ESP_LOGI(TAG, "product 0x%08lX, serial 0x%016llX%s",
                 (unsigned long)id.product_number, (unsigned long long)id.serial_number,
                 is_sdp810_125pa ? " - confirmed SDP810-125Pa" : " - NOT an SDP810-125Pa");
    } else {
        ESP_LOGW(TAG, "failed to read product identifier");
    }

    if (!is_sdp810_125pa)
        return false;

    // Start continuous measurement: mass flow, no averaging
    uint8_t cmd[2] = {0x36, 0x03};
    esp_err_t r = i2c_master_transmit(dev, cmd, 2, 50);
    vTaskDelay(pdMS_TO_TICKS(20));
    return r == ESP_OK;
}

float sdp800_read(i2c_master_dev_handle_t dev)
{
    uint8_t rx[9];
    if (i2c_master_receive(dev, rx, 9, 50) != ESP_OK) return 0.0f;
    int16_t raw   = (int16_t)(((uint16_t)rx[0] << 8) | rx[1]);
    int16_t scale = (int16_t)(((uint16_t)rx[6] << 8) | rx[7]);
    if (scale == 0) return 0.0f;
    return (float)raw / (float)scale;
}
