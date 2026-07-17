#pragma once
#include "driver/i2c_master.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Product number + serial number reported by the "read product identifier"
// command sequence (SDP8xx datasheet §5.5).
typedef struct {
    uint32_t product_number;
    uint64_t serial_number;
} sdp800_id_t;

// SDP810-125Pa product number (datasheet §6.3.6). The low byte is a revision
// number that "can be subject to change" per the datasheet, so mask it off
// with SDP8XX_PRODUCT_NUMBER_VARIANT_MASK before comparing.
#define SDP810_125PA_PRODUCT_NUMBER       0x03020200u
#define SDP8XX_PRODUCT_NUMBER_VARIANT_MASK 0xFFFFFF00u

bool sdp800_init(i2c_master_dev_handle_t dev);
float sdp800_read(i2c_master_dev_handle_t dev);
bool sdp800_read_product_id(i2c_master_dev_handle_t dev, sdp800_id_t *out);

#ifdef __cplusplus
}
#endif
