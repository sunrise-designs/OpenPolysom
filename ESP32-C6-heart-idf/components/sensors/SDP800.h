#pragma once
#include "driver/i2c_master.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool sdp800_init(i2c_master_dev_handle_t dev);
float sdp800_read(i2c_master_dev_handle_t dev);

#ifdef __cplusplus
}
#endif
