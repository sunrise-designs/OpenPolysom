#include "SDP800.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

bool sdp800_init(i2c_master_dev_handle_t dev)
{
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
