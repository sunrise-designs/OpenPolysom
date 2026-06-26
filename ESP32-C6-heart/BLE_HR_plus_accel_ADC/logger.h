#pragma once
#include <SD.h>
#include <SPI.h>
#include <stdint.h>

// ── SD card SPI pins ──────────────────────────────────────────────────────────
// Set these to match your wiring on the ESP32-C6-LCD-1.47 board.
#define SD_CS_PIN   10
#define SD_SCK_PIN  13
#define SD_MOSI_PIN 11
#define SD_MISO_PIN 12

// ── File paths ────────────────────────────────────────────────────────────────
// VFS path used by edflib (fopen); Arduino SD path used for remove/exists.
// The default VFS mount point for SD.begin() in ESP32 Arduino is "/sd".
// Change to "/sdcard" if your core version uses that prefix.
#define EDF_FILE_VFS  "/sd/biometric.edf"
#define EDF_FILE_FS   "/biometric.edf"

// ── Timing ────────────────────────────────────────────────────────────────────
#define LOG_RATE_MS       20    // 50 Hz
#define RECORD_DURATION_S 10   // seconds per EDF data record
#define SAMPLES_50HZ      500  // 50 Hz × 10 s
#define SAMPLES_1HZ       10   // 1 Hz × 10 s

extern uint32_t      recordCount;
extern bool          loggingActive;
extern volatile bool dumping;
extern bool          wokeFromSleep;

void initLogger();

// LDC1612 baseline accessors (used by display module)
uint32_t getLdc0Baseline();
uint32_t getLdc1Baseline();
bool     getLdcBaselineOk();

void logRecord(int16_t  a0x, int16_t  a0y, int16_t  a0z,
               int16_t  a1x, int16_t  a1y, int16_t  a1z,
               uint32_t ldc0, uint32_t ldc1,
               uint16_t ecg,  float    pressure_mbar,
               uint16_t rr_ms);

void processLogCommand(char cmd);
void closeLogger();
