#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool        ble_connected;
    const char *ble_device_name;
    uint16_t    bpm;
    uint16_t    rr_ms;
    int16_t     accel0[3];
    int16_t     accel1[3];
    uint32_t    ldc0;
    uint32_t    ldc1;
    uint32_t    ldc0_baseline;
    uint32_t    ldc1_baseline;
    bool        baseline_ok;
    bool        recording;
} display_data_t;

// Initialise the SPI bus, ST7789 panel, and draw static labels.
// Must be called BEFORE logger_init() because it initialises the shared SPI bus.
void display_init(void);

// Redraw all live values from data. Call at 5 Hz from the sensor task.
void display_update(const display_data_t *data);

#ifdef __cplusplus
}
#endif
