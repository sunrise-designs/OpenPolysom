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

// Initialise the I2C bus and all sensors. Returns false if any sensor fails.
bool sensors_init(void);

// Read all sensors (call at 50 Hz from sensor_task).
void sensors_read(void);

// Read ECG ADC (call at 100 Hz from sensor_task).
void sensors_read_ecg(void);

#ifdef __cplusplus
}
#endif
