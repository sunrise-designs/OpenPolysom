#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Shared sensor data (written by sensor_task, read by logger/display) ───────
extern volatile int16_t  g_accel0_x, g_accel0_y, g_accel0_z;
extern volatile int16_t  g_accel1_x, g_accel1_y, g_accel1_z;
extern volatile uint32_t g_ldc0, g_ldc1;
extern volatile float    g_pressure_mbar;
extern volatile uint16_t g_ecg_raw;

// True once sensors_init() has detected a DS1307 RTC on the I2C bus.
extern volatile bool g_rtc_present;

// Per-sensor presence, set by sensors_init(). Used to build the recording
// sidecar metadata (see logger.cpp).
extern volatile bool g_mma0_present;
extern volatile bool g_mma1_present;
extern volatile bool g_ldc_present;
extern volatile bool g_sdp_present;

// Initialise the I2C bus and all sensors. Returns false if any sensor fails.
bool sensors_init(void);

// Read all sensors (call at 50 Hz from sensor_task).
void sensors_read(void);

// Read ECG ADC (call at 100 Hz from sensor_task).
void sensors_read_ecg(void);

// If the DS1307 RTC is present and holds a plausible time (year >= 2026,
// i.e. it has already been set), applies it to the system clock (as UTC,
// then sets the local TZ) and returns true. Meant to be called once at
// startup in place of wifi_ntp_sync().
bool sensors_rtc_startup_sync(void);

// Writes the current system time (UTC) to the RTC, if present. Call once
// after a successful Wi-Fi/NTP sync so future boots can use the RTC instead.
void sensors_rtc_write_from_system(void);

// Re-reads the RTC, if present, and re-applies it to the system clock to
// correct drift in the ESP32's internal oscillator. Call periodically.
void sensors_rtc_periodic_sync(void);

#ifdef __cplusplus
}
#endif
