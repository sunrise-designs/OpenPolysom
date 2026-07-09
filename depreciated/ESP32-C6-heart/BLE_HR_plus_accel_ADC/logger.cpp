#include "logger.h"
#include "edflib.h"
#include <Arduino.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

uint32_t      recordCount   = 0;
bool          loggingActive = false;
volatile bool dumping       = false;

static int edfHandle = -1;

// ── EDF signal indices ────────────────────────────────────────────────────────
#define SIG_THORACIC 0   // LDC1612 CH0 @ 50 Hz
#define SIG_ABDOMEN  1   // LDC1612 CH1 @ 50 Hz
#define SIG_FLOW     2   // SDP800 pressure @ 50 Hz
#define SIG_ECG      3   // ADC ECG @ 50 Hz
#define SIG_A0X      4   // MMA8451 chip0 X @ 50 Hz
#define SIG_A0Y      5
#define SIG_A0Z      6
#define SIG_A1X      7   // MMA8451 chip1 X @ 50 Hz
#define SIG_A1Y      8
#define SIG_A1Z      9
#define SIG_RR       10  // BLE RR interval @ 1 Hz
#define NUM_SIGNALS  11

// ── Sample buffers (filled at 50 Hz, flushed every 10 s) ─────────────────────
static int thoracicBuf[SAMPLES_50HZ];
static int abdomenBuf[SAMPLES_50HZ];
static int flowBuf[SAMPLES_50HZ];
static int ecgBuf[SAMPLES_50HZ];
static int a0xBuf[SAMPLES_50HZ], a0yBuf[SAMPLES_50HZ], a0zBuf[SAMPLES_50HZ];
static int a1xBuf[SAMPLES_50HZ], a1yBuf[SAMPLES_50HZ], a1zBuf[SAMPLES_50HZ];
static int rrBuf[SAMPLES_1HZ];

static int sampleIdx = 0;
static int rrIdx     = 0;

// ── LDC baseline (subtracted before storing; reset on 'E') ───────────────────
static uint32_t ldc0_baseline   = 0;
static uint32_t ldc1_baseline   = 0;
static bool     ldc_baseline_ok = false;

static SPIClass SD_SPI(FSPI);

static bool openEdf(bool overwrite) {
    if (!overwrite && SD.exists(EDF_FILE_FS)) {
        Serial.println("EDF: existing file found. Send 'E' to erase and restart.");
        loggingActive = false;
        return false;
    }

    edfHandle = edfopen_file_writeonly(EDF_FILE_VFS, EDFLIB_FILETYPE_EDFPLUS, NUM_SIGNALS);
    if (edfHandle < 0) {
        Serial.printf("EDF: open failed (%d)\n", edfHandle);
        return false;
    }

    // 10-second data records: 1 000 000 × 10 µs = 10 s
    edf_set_datarecord_duration(edfHandle, 1000000);

    struct SigDef {
        const char *label;
        const char *transducer;
        const char *dim;
        int         rate;
        int         dmax;
        int         dmin;
        double      pmax;
        double      pmin;
    };

    const SigDef sigs[NUM_SIGNALS] = {
        { "Thoracic", "LDC1612 CH0",   "counts", 50,  32767, -32767,  1000000.0, -1000000.0 },
        { "Abdomen",  "LDC1612 CH1",   "counts", 50,  32767, -32767,  1000000.0, -1000000.0 },
        { "Flow",     "SDP800-125Pa",  "mbar",   50,  32767, -32767,       2.0,       -2.0  },
        { "ECG",      "AD8232 ADC",    "ADC",    50,   4095,      0,    4095.0,        0.0  },
        { "Accel0X",  "MMA8451 chip0", "mg",     50,   8191,  -8192,       2000.0, -2000.0 },
        { "Accel0Y",  "MMA8451 chip0", "mg",     50,   8191,  -8192,       2000.0, -2000.0 },
        { "Accel0Z",  "MMA8451 chip0", "mg",     50,   8191,  -8192,       2000.0, -2000.0 },
        { "Accel1X",  "MMA8451 chip1", "mg",     50,   8191,  -8192,       2000.0, -2000.0 },
        { "Accel1Y",  "MMA8451 chip1", "mg",     50,   8191,  -8192,       2000.0, -2000.0 },
        { "Accel1Z",  "MMA8451 chip1", "mg",     50,   8191,  -8192,       2000.0, -2000.0 },
        { "RR",       "Polar H9 BLE",  "ms",      1,   2000,      0,    2000.0,        0.0  },
    };

    for (int i = 0; i < NUM_SIGNALS; i++) {
        edf_set_label(edfHandle, i, sigs[i].label);
        edf_set_transducer(edfHandle, i, sigs[i].transducer);
        edf_set_samplefrequency(edfHandle, i, sigs[i].rate);
        edf_set_digital_maximum(edfHandle, i, sigs[i].dmax);
        edf_set_digital_minimum(edfHandle, i, sigs[i].dmin);
        edf_set_physical_maximum(edfHandle, i, sigs[i].pmax);
        edf_set_physical_minimum(edfHandle, i, sigs[i].pmin);
        edf_set_physical_dimension(edfHandle, i, sigs[i].dim);
    }

    struct tm t;
    if (getLocalTime(&t)) {
        edf_set_startdatetime(edfHandle,
                              t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                              t.tm_hour, t.tm_min, t.tm_sec);
    }

    sampleIdx     = 0;
    rrIdx         = 0;
    recordCount   = 0;
    loggingActive = true;
    Serial.println("EDF: recording started");
    return true;
}

void initLogger() {
    SD_SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    if (!SD.begin(SD_CS_PIN, SD_SPI)) {
        Serial.println("SD: mount failed — check pins and card");
        return;
    }
    Serial.printf("SD: %.0f MB free\n",
                  (float)(SD.totalBytes() - SD.usedBytes()) / 1048576.0f);
    openEdf(false);
}

void logRecord(int16_t  a0x, int16_t  a0y, int16_t  a0z,
               int16_t  a1x, int16_t  a1y, int16_t  a1z,
               uint32_t ldc0, uint32_t ldc1,
               uint16_t ecg,  float    pressure_mbar,
               uint16_t rr_ms) {
    if (!loggingActive || edfHandle < 0) return;

    // Establish LDC baseline from the first valid (non-zero) reading
    if (!ldc_baseline_ok && ldc0 != 0) {
        ldc0_baseline   = ldc0;
        ldc1_baseline   = ldc1;
        ldc_baseline_ok = true;
    }

    // LDC: signed difference from baseline, scaled to ±32767 over ±1 000 000 counts
    int32_t d0 = (int32_t)(ldc0 - ldc0_baseline);
    int32_t d1 = (int32_t)(ldc1 - ldc1_baseline);
    thoracicBuf[sampleIdx] = constrain(d0 / 30, -32767, 32767);
    abdomenBuf[sampleIdx]  = constrain(d1 / 30, -32767, 32767);

    // Pressure: scale ±2 mbar → ±32767
    flowBuf[sampleIdx] = constrain((int)(pressure_mbar * 32767.0f / 2.0f), -32767, 32767);

    ecgBuf[sampleIdx] = (int)ecg;

    a0xBuf[sampleIdx] = a0x;
    a0yBuf[sampleIdx] = a0y;
    a0zBuf[sampleIdx] = a0z;
    a1xBuf[sampleIdx] = a1x;
    a1yBuf[sampleIdx] = a1y;
    a1zBuf[sampleIdx] = a1z;

    // RR: 1 sample per second (every 50 accel samples at 50 Hz)
    if (sampleIdx % 50 == 49) {
        rrBuf[rrIdx++] = (int)rr_ms;
    }

    sampleIdx++;

    if (sampleIdx >= SAMPLES_50HZ) {
        edfwrite_digital_samples(edfHandle, thoracicBuf);
        edfwrite_digital_samples(edfHandle, abdomenBuf);
        edfwrite_digital_samples(edfHandle, flowBuf);
        edfwrite_digital_samples(edfHandle, ecgBuf);
        edfwrite_digital_samples(edfHandle, a0xBuf);
        edfwrite_digital_samples(edfHandle, a0yBuf);
        edfwrite_digital_samples(edfHandle, a0zBuf);
        edfwrite_digital_samples(edfHandle, a1xBuf);
        edfwrite_digital_samples(edfHandle, a1yBuf);
        edfwrite_digital_samples(edfHandle, a1zBuf);
        edfwrite_digital_samples(edfHandle, rrBuf);

        sampleIdx = 0;
        rrIdx     = 0;
        recordCount++;

        // Flush to SD every 10 records (every 100 s)
        if (recordCount % 10 == 0) {
            edfflush_file(edfHandle, (int)recordCount);
            Serial.printf("EDF: %lu records (%.1f min)\n",
                          recordCount, recordCount * RECORD_DURATION_S / 60.0f);
        }
    }
}

void processLogCommand(char cmd) {
    if (cmd == 'E') {
        closeLogger();
        SD.remove(EDF_FILE_FS);
        ldc_baseline_ok = false;
        openEdf(true);
    }
}

uint32_t getLdc0Baseline() { return ldc0_baseline; }
uint32_t getLdc1Baseline() { return ldc1_baseline; }
bool     getLdcBaselineOk() { return ldc_baseline_ok; }

void closeLogger() {
    if (edfHandle >= 0) {
        edfclose_file(edfHandle);
        edfHandle     = -1;
        loggingActive = false;
    }
}
