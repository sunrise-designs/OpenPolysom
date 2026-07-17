#pragma once
#include "driver/i2c_master.h"
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Probes the DS3231 by attempting to read its seconds register.
bool ds3231_init(i2c_master_dev_handle_t dev);

// Reads the current time from the RTC into *out, as UTC (this project always
// writes UTC to the RTC — see ds3231_set_time). Returns false if the RTC's
// oscillator-stop flag is set (OSF bit in the control/status register —
// meaning the clock has never been set, or lost power at some point since)
// or the I2C transaction fails.
bool ds3231_get_time(i2c_master_dev_handle_t dev, struct tm *out);

// Writes *t (interpreted as UTC) to the RTC in 24-hour mode and clears the
// oscillator-stop flag (OSF), marking the time as valid.
bool ds3231_set_time(i2c_master_dev_handle_t dev, const struct tm *t);

#ifdef __cplusplus
}
#endif
