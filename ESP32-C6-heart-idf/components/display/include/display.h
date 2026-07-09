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
    float       pressure_mbar;
    bool        recording;
    uint32_t    recording_seconds;
    const char *wifi_ssid;  // "" if Wi-Fi wasn't used/connected at boot (e.g. RTC sync)
    bool        ap_active;  // true once the download-button file-server AP is up
    uint8_t     batt_percent;
} display_data_t;

// Initialise the SPI bus, ST7789 panel, and draw static labels.
// Must be called BEFORE logger_init() because it initialises the shared SPI bus.
void display_init(void);

// Redraw all live values from data. Call at 5 Hz from the sensor task.
void display_update(const display_data_t *data);

// Draw a one-line message in the boot-info area at the bottom of the screen
// (below everything display_update() touches, so it stays visible until the
// next reboot). Used to show the reset reason when no serial monitor is
// attached.
void display_boot_msg(const char *msg);

// Full display shutdown: backlight off, panel put into ST7789 sleep mode
// (SLPIN). display_update()/display_boot_msg() become no-ops until
// display_wake() is called. Idempotent.
void display_sleep(void);

// Reverse of display_sleep(): panel SLPOUT + display on + backlight restored,
// static labels redrawn. Idempotent.
void display_wake(void);

// True after display_sleep(), false initially and after display_wake().
bool display_is_sleeping(void);

#ifdef __cplusplus
}
#endif
