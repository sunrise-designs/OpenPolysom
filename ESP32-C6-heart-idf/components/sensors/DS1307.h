#pragma once
#include "driver/i2c_master.h"
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Probes the DS1307 by attempting to read its seconds register.
bool ds1307_init(i2c_master_dev_handle_t dev);

// Reads the current time from the RTC into *out, as UTC (this project always
// writes UTC to the RTC — see ds1307_set_time). Returns false if the RTC's
// oscillator is halted (CH bit set — clock has never been set) or the I2C
// transaction fails.
bool ds1307_get_time(i2c_master_dev_handle_t dev, struct tm *out);

// Writes *t (interpreted as UTC) to the RTC in 24-hour mode and starts its
// oscillator (clears the CH bit).
bool ds1307_set_time(i2c_master_dev_handle_t dev, const struct tm *t);

#ifdef __cplusplus
}
#endif
