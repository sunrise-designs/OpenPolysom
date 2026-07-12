#pragma once
#include "driver/i2c_master.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t     accel0[3];
    int16_t     accel1[3];
    uint32_t    ldc0;
    uint32_t    ldc1;
    float       pressure_mbar;
    bool        recording;
    uint32_t    recording_seconds;
    const char *time_sync_source;  // "RTC", "Serial", or "none" — see main.cpp
    uint8_t     batt_percent;
} display_data_t;

// Initialise the shared I2C bus and the SH1106 OLED, and draw static labels.
// Must be called BEFORE sensors_init() — it creates the shared I2C bus that
// sensors_init() adds its own devices to (see display_get_i2c_bus()).
void display_init(void);

// The shared I2C bus display_init() creates. sensors_init() adds the
// MMA8451/LDC1612/SDP800/DS1307 devices onto this same bus rather than
// creating a second one, since only one master bus handle can own a port.
i2c_master_bus_handle_t display_get_i2c_bus(void);

// Redraw all live values from data. Call at 5 Hz from the sensor task.
void display_update(const display_data_t *data);

// Draw a one-line message in the boot-info area at the bottom of the screen
// (below everything display_update() touches, so it stays visible until the
// next reboot). Used to show the reset reason when no serial monitor is
// attached.
void display_boot_msg(const char *msg);

// Full display shutdown: SH1106 put into sleep mode (display OFF). No
// backlight to switch (OLED is emissive). display_update()/display_boot_msg()
// become no-ops until display_wake() is called. Idempotent.
void display_sleep(void);

// Reverse of display_sleep(): SH1106 display ON, static labels redrawn.
// Idempotent.
void display_wake(void);

// True after display_sleep(), false initially and after display_wake().
bool display_is_sleeping(void);

#ifdef __cplusplus
}
#endif
