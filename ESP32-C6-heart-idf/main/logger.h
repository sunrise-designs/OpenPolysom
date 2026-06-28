#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECORD_DURATION_S  10
#define SAMPLES_50HZ      500
#define SAMPLES_1HZ        10

// Initialise SD card and open EDF file for writing.
bool logger_init(void);

// Append one 50 Hz sample. Flushes to SD automatically every 10 EDF records.
void logger_record(int16_t  a0x, int16_t  a0y, int16_t  a0z,
                   int16_t  a1x, int16_t  a1y, int16_t  a1z,
                   uint32_t ldc0, uint32_t ldc1,
                   uint16_t ecg,  float    pressure_mbar,
                   uint16_t rr_ms);

// Process a serial command character ('E' erases and restarts the EDF file).
void logger_process_cmd(char cmd);

// Close the EDF file cleanly (call before deep sleep).
void logger_close(void);

// LDC baseline accessors used by the display module.
uint32_t logger_get_ldc0_baseline(void);
uint32_t logger_get_ldc1_baseline(void);
bool     logger_get_baseline_ok(void);

extern volatile bool g_dumping;

#ifdef __cplusplus
}
#endif
