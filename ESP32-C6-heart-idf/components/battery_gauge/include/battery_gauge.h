#pragma once

#include "esp_adc/adc_oneshot.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void battery_gauge_init(adc_oneshot_unit_handle_t adc_unit);
int battery_gauge_get_voltage_mv(void);
uint8_t battery_gauge_get_percentage(void);

#ifdef __cplusplus
}
#endif
