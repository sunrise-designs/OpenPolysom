#pragma once
#include <LittleFS.h>

// EDF+ file written via edflib using the ESP32 VFS layer (fopen path below).
// Signals: AccelX, AccelY, AccelZ @ 10 Hz  |  RR interval @ 1 Hz
// 10-second data records → 2160 records = 6 h, fitting the 1.5 MB LittleFS partition.
#define EDF_FILE_VFS      "/littlefs/biometric.edf"  // POSIX path (edflib / fopen)
#define EDF_FILE_FS       "/biometric.edf"            // Arduino FS path (LittleFS.remove)
#define LOG_RATE_MS        100   // 10 Hz — call rate for logRecord()
#define RECORD_DURATION_S   10   // seconds per EDF data record
#define SAMPLES_PER_REC    100   // AccelX/Y/Z samples per record (10 Hz × 10 s)
#define RR_PER_REC          10   // RR samples per record (1 Hz × 10 s)
#define MAX_RECORDS       2160UL // 6 hours at 1 record / 10 s

extern uint32_t      recordCount;
extern bool          loggingActive;
extern volatile bool dumping;
extern bool          wokeFromSleep;  // defined in main .ino

void initLogger();
void logRecord(uint16_t accelX, uint16_t accelY, uint16_t accelZ, uint16_t rr_ms);
void processLogCommand(char cmd);
void closeLogger();
