#pragma once
#include "driver/i2c_master.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// TMP117 I2C addresses, selected by what the ADD0 pin is tied to
// (TMP117 datasheet §7.5.3.1, Table 7-2). Two devices on one bus must use
// two different straps.
#define TMP117_ADDR_GND  0x48
#define TMP117_ADDR_VCC  0x49
#define TMP117_ADDR_SDA  0x4A
#define TMP117_ADDR_SCL  0x4B

// One per physical sensor. The struct holds the per-device log state, so a
// failing sensor does not hide (or trigger) messages about the other one.
typedef struct {
    i2c_master_dev_handle_t dev;
    uint8_t addr;           // for log messages only
    bool    read_failing;   // the last read failed, and that was already logged
} tmp117_t;

// Checks the device ID and puts the sensor in continuous conversion mode.
// `addr` is the address `dev` was added with; it is used only in log messages.
// Returns false if the device isn't a TMP117 or doesn't respond.
bool tmp117_init(tmp117_t *s, i2c_master_dev_handle_t dev, uint8_t addr);

// Reads the latest conversion result, in degrees Celsius. Returns false, with
// *out_c unchanged, if the read fails or no conversion has finished yet since
// power-up. 0 °C is a valid reading, so failure can't be a 0.0f return value.
bool tmp117_read(tmp117_t *s, float *out_c);

#ifdef __cplusplus
}
#endif
