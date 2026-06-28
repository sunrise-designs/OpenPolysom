#pragma once
#include "driver/i2c_master.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool ldc1612_init(i2c_master_dev_handle_t dev);
uint32_t ldc1612_read_channel(i2c_master_dev_handle_t dev, uint8_t ch);

#ifdef __cplusplus
}
#endif
