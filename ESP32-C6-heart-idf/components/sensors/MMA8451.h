#pragma once
#include "driver/i2c_master.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool mma8451_init(i2c_master_dev_handle_t dev);
void mma8451_read(i2c_master_dev_handle_t dev,
                  volatile int16_t *ox, volatile int16_t *oy, volatile int16_t *oz);

#ifdef __cplusplus
}
#endif
