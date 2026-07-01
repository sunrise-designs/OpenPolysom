#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECORD_DURATION_S  10
#define SAMPLES_50HZ      500
#define SAMPLES_1HZ        10

// Capture ESP_LOG output to a buffered SD file. Call once, first thing in
// app_main, before any other init, so early boot logs aren't missed.
// Logs are still echoed to the console as before; the SD write only happens
// when the 4 KB RAM buffer fills or at existing safe flush points, to limit
// SD card wear.
void logger_log_init(void);

// Initialise SD card and open EDF file for writing.
bool logger_init(void);

// Append one 50 Hz sample. Flushes to SD automatically every 10 EDF records.
void logger_record(int16_t  a0x, int16_t  a0y, int16_t  a0z,
                   int16_t  a1x, int16_t  a1y, int16_t  a1z,
                   uint32_t ldc0, uint32_t ldc1,
                   uint16_t ecg,  float    pressure_mbar,
                   uint16_t rr_ms);

// Format the SD card as FAT32. Closes any open EDF file first.
// Mounts the card if not already mounted. Does NOT reopen the EDF file.
bool logger_format_sd(void);

// Close the EDF file cleanly (call before deep sleep).
void logger_close(void);

// LDC baseline accessors used by the display module.
uint32_t logger_get_ldc0_baseline(void);
uint32_t logger_get_ldc1_baseline(void);
bool     logger_get_baseline_ok(void);

// True once logger_init() has successfully mounted the SD card and opened the
// EDF file. False if logger_init() failed (e.g. no SD card present) — used by
// the display module to warn that samples are not being recorded.
bool     logger_is_active(void);

#ifdef __cplusplus
}
#endif
