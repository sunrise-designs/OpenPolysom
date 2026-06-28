#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialise the SPI bus, ST7789 panel, and draw static labels.
// Must be called BEFORE logger_init() because it initialises the shared SPI bus.
void display_init(void);

// Redraw all live values. Call at 5 Hz from the sensor task.
void display_update(void);

#ifdef __cplusplus
}
#endif
