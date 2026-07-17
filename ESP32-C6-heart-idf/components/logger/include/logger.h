#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECORD_DURATION_S  10
#define SAMPLES_50HZ      500
#define SAMPLES_RR_HZ      25  // RR channel rate: 2.5 Hz x 10 s record
#define SAMPLES_100HZ    1000  // ECG channel rate: 100 Hz x 10 s record

// Capture ESP_LOG output to a buffered SD file. Call once, first thing in
// app_main, before any other init, so early boot logs aren't missed.
// Logs are still echoed to the console as before; the SD write only happens
// when the 4 KB RAM buffer fills or at existing safe flush points, to limit
// SD card wear. ESP_LOGW/ESP_LOGE lines are the exception and reach the card
// immediately — a fault that stops the firmware before the next flush point
// would otherwise take its own explanation down with it.
void logger_log_init(void);

// Initialise SD card and open EDF file for writing.
bool logger_init(void);

// Append one 50 Hz sample. Flushes to SD automatically every 10 EDF records.
void logger_record(int16_t  a0x, int16_t  a0y, int16_t  a0z,
                   int16_t  a1x, int16_t  a1y, int16_t  a1z,
                   uint32_t ldc0, uint32_t ldc1,
                   float    pressure_mbar,
                   uint16_t rr_ms);

// Append one 100 Hz ECG sample (AD8232, ADC channel 0, raw counts). Call
// every 10 ms tick, independently of logger_record()'s 50 Hz cadence.
void logger_record_ecg(uint16_t ecg_raw);

// Format the SD card as FAT32. Closes any open EDF file first.
// Mounts the card if not already mounted. Does NOT reopen the EDF file.
bool logger_format_sd(void);

// Close the EDF file cleanly (call before deep sleep).
void logger_close(void);

// LDC baseline accessors (thoracic/abdomen delta calibration; no longer
// shown on the OLED display, which only has room for raw CH0/CH1 counts).
uint32_t logger_get_ldc0_baseline(void);
uint32_t logger_get_ldc1_baseline(void);
bool     logger_get_baseline_ok(void);

// True once logger_init() has successfully mounted the SD card and opened the
// EDF file. False if logger_init() failed (e.g. no SD card present), or if a
// recording stopped itself after an SD write failure (see
// logger_had_write_error) — used by the display module to warn that samples are
// not being recorded.
bool     logger_is_active(void);

// True if a recording started but then stopped because the SD card rejected a
// write. Distinguishes "recording aborted mid-study" from the never-started
// case that logger_is_active() alone reports. The EDF is valid up to its last
// flushed record; the reason is in the .log file beside it.
bool     logger_had_write_error(void);

// Seconds elapsed since the current recording started. 0 if not recording.
uint32_t logger_get_elapsed_seconds(void);

#ifdef __cplusplus
}
#endif
