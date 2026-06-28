#include "MMA8451.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MMA_REG_OUT_X_MSB   0x01
#define MMA_REG_WHOAMI      0x0D
#define MMA_REG_XYZ_CFG     0x0E
#define MMA_REG_PL_CFG      0x11
#define MMA_REG_CTRL1       0x2A
#define MMA_REG_CTRL2       0x2B
#define MMA_WHOAMI_VAL      0x1A
#define MMA_CTRL1_50HZ_ACTIVE  0x21  // ODR=50Hz (DR=010), ACTIVE bit

static esp_err_t i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, 2, 50);
}

static esp_err_t i2c_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *rx, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, rx, len, 50);
}

bool mma8451_init(i2c_master_dev_handle_t dev)
{
    uint8_t id = 0;
    if (i2c_read_reg(dev, MMA_REG_WHOAMI, &id, 1) != ESP_OK || id != MMA_WHOAMI_VAL) {
        return false;
    }
    i2c_write_reg(dev, MMA_REG_CTRL1, 0x00);
    i2c_write_reg(dev, MMA_REG_CTRL2, 0x40);  // RST bit
    vTaskDelay(pdMS_TO_TICKS(2));
    i2c_write_reg(dev, MMA_REG_CTRL2,   0x02);  // high-res mode
    i2c_write_reg(dev, MMA_REG_PL_CFG,  0x40);  // portrait detection enable
    i2c_write_reg(dev, MMA_REG_XYZ_CFG, 0x00);  // ±2G range
    i2c_write_reg(dev, MMA_REG_CTRL1,   MMA_CTRL1_50HZ_ACTIVE);
    return true;
}

void mma8451_read(i2c_master_dev_handle_t dev,
                  volatile int16_t *ox, volatile int16_t *oy, volatile int16_t *oz)
{
    uint8_t buf[6];
    if (i2c_read_reg(dev, MMA_REG_OUT_X_MSB, buf, 6) != ESP_OK) return;
    *ox = (int16_t)(((uint16_t)buf[0] << 8) | buf[1]) >> 2;
    *oy = (int16_t)(((uint16_t)buf[2] << 8) | buf[3]) >> 2;
    *oz = (int16_t)(((uint16_t)buf[4] << 8) | buf[5]) >> 2;
}
