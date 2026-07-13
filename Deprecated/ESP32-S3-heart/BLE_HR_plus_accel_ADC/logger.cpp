#include "logger.h"
#include "edflib.h"
#include <Arduino.h>
#include <time.h>
#include <stdio.h>

uint32_t      recordCount   = 0;
bool          loggingActive = false;
volatile bool dumping       = false;

static int edfHandle = -1;

// Sample buffers — filled at 10 Hz, flushed every RECORD_DURATION_S seconds.
static int accelXBuf[SAMPLES_PER_REC];
static int accelYBuf[SAMPLES_PER_REC];
static int accelZBuf[SAMPLES_PER_REC];
static int rrBuf[RR_PER_REC];
static int sampleIdx   = 0;  // 0 .. SAMPLES_PER_REC-1
static int rrSampleIdx = 0;  // 0 .. RR_PER_REC-1

// ---- Signal indices ----
#define SIG_ACCEL_X  0
#define SIG_ACCEL_Y  1
#define SIG_ACCEL_Z  2
#define SIG_RR       3
#define NUM_SIGNALS  4

static bool openEdf(bool overwrite) {
  if (!overwrite && LittleFS.exists(EDF_FILE_FS)) {
    // Existing file: can't resume EDF mid-stream, so treat it as complete.
    Serial.println("EDF: existing file found (previous session). Erase with 'E' to start fresh.");
    loggingActive = false;
    return false;
  }

  edfHandle = edfopen_file_writeonly(EDF_FILE_VFS, EDFLIB_FILETYPE_EDFPLUS, NUM_SIGNALS);
  if (edfHandle < 0) {
    Serial.printf("EDF: open failed (%d)\n", edfHandle);
    return false;
  }

  // 10-second data records: duration in units of 10 µs → 10 s = 1 000 000 × 10 µs
  edf_set_datarecord_duration(edfHandle, 1000000);

  // Signal 0: AccelX  — 10 Hz, ADC 12-bit (0-4095), physical 0-3300 mV
  edf_set_samplefrequency(edfHandle, SIG_ACCEL_X, 10);
  edf_set_digital_maximum(edfHandle, SIG_ACCEL_X, 4095);
  edf_set_digital_minimum(edfHandle, SIG_ACCEL_X, 0);
  edf_set_physical_maximum(edfHandle, SIG_ACCEL_X, 3300.0);
  edf_set_physical_minimum(edfHandle, SIG_ACCEL_X, 0.0);
  edf_set_label(edfHandle, SIG_ACCEL_X, "AccelX");
  edf_set_physical_dimension(edfHandle, SIG_ACCEL_X, "mV");

  // Signal 1: AccelY
  edf_set_samplefrequency(edfHandle, SIG_ACCEL_Y, 10);
  edf_set_digital_maximum(edfHandle, SIG_ACCEL_Y, 4095);
  edf_set_digital_minimum(edfHandle, SIG_ACCEL_Y, 0);
  edf_set_physical_maximum(edfHandle, SIG_ACCEL_Y, 3300.0);
  edf_set_physical_minimum(edfHandle, SIG_ACCEL_Y, 0.0);
  edf_set_label(edfHandle, SIG_ACCEL_Y, "AccelY");
  edf_set_physical_dimension(edfHandle, SIG_ACCEL_Y, "mV");

  // Signal 2: AccelZ
  edf_set_samplefrequency(edfHandle, SIG_ACCEL_Z, 10);
  edf_set_digital_maximum(edfHandle, SIG_ACCEL_Z, 4095);
  edf_set_digital_minimum(edfHandle, SIG_ACCEL_Z, 0);
  edf_set_physical_maximum(edfHandle, SIG_ACCEL_Z, 3300.0);
  edf_set_physical_minimum(edfHandle, SIG_ACCEL_Z, 0.0);
  edf_set_label(edfHandle, SIG_ACCEL_Z, "AccelZ");
  edf_set_physical_dimension(edfHandle, SIG_ACCEL_Z, "mV");

  // Signal 3: RR interval — 1 Hz (1 sample/second, 10 per 10-second record)
  edf_set_samplefrequency(edfHandle, SIG_RR, 1);
  edf_set_digital_maximum(edfHandle, SIG_RR, 2000);
  edf_set_digital_minimum(edfHandle, SIG_RR, 0);
  edf_set_physical_maximum(edfHandle, SIG_RR, 2000.0);
  edf_set_physical_minimum(edfHandle, SIG_RR, 0.0);
  edf_set_label(edfHandle, SIG_RR, "RR");
  edf_set_physical_dimension(edfHandle, SIG_RR, "ms");

  // Start date/time from NTP-synced clock
  struct tm t;
  if (getLocalTime(&t)) {
    edf_set_startdatetime(edfHandle,
                          t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                          t.tm_hour, t.tm_min, t.tm_sec);
  }

  sampleIdx   = 0;
  rrSampleIdx = 0;
  recordCount = 0;
  loggingActive = true;
  Serial.println("EDF: recording started");
  return true;
}

void initLogger() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS: mount failed");
    return;
  }
  openEdf(false);  // keep existing file if present; user can erase with 'E'
}

void logRecord(uint16_t accelX, uint16_t accelY, uint16_t accelZ, uint16_t rr_ms) {
  if (!loggingActive || edfHandle < 0) return;

  accelXBuf[sampleIdx] = (int)accelX;
  accelYBuf[sampleIdx] = (int)accelY;
  accelZBuf[sampleIdx] = (int)accelZ;

  // RR is sampled once per second (every 10 accel samples)
  if (sampleIdx % 10 == 9) {
    rrBuf[rrSampleIdx++] = (int)rr_ms;
  }

  sampleIdx++;

  if (sampleIdx >= SAMPLES_PER_REC) {
    edfwrite_digital_samples(edfHandle, accelXBuf);
    edfwrite_digital_samples(edfHandle, accelYBuf);
    edfwrite_digital_samples(edfHandle, accelZBuf);
    edfwrite_digital_samples(edfHandle, rrBuf);

    sampleIdx   = 0;
    rrSampleIdx = 0;
    recordCount++;

    if (recordCount >= MAX_RECORDS) {
      edfclose_file(edfHandle);
      edfHandle     = -1;
      loggingActive = false;
      Serial.println("EDF: log full (6 hours).");
    }
  }
}

void processLogCommand(char cmd) {
  if (cmd == 'E') {
    closeLogger();
    LittleFS.remove(EDF_FILE_FS);
    openEdf(true);

  } else if (cmd == 'D') {
    dumping = true;
    closeLogger();

    FILE *rf = fopen(EDF_FILE_VFS, "rb");
    if (rf) {
      fseek(rf, 0, SEEK_END);
      long sz = ftell(rf);
      fseek(rf, 0, SEEK_SET);
      Serial.printf("DUMP_START:%ld\n", sz);
      uint8_t buf[64];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), rf)) > 0) {
        for (size_t i = 0; i < n; i++) Serial.printf("%02X", buf[i]);
      }
      fclose(rf);
    } else {
      Serial.print("DUMP_START:0\n");
    }
    Serial.print("\nDUMP_END\n");
    dumping = false;

    // Reopen for continued recording
    openEdf(false);
  }
}

void closeLogger() {
  if (edfHandle >= 0) {
    edfclose_file(edfHandle);
    edfHandle     = -1;
    loggingActive = false;
  }
}
