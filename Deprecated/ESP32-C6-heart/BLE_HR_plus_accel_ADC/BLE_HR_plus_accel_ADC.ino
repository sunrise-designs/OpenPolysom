#include "Wire.h"
#include "Adafruit_MMA8451.h"
#include "Adafruit_Sensor.h"
#include "LDC1612.h"
#include "Sdp800.h"
#include "esp_sleep.h"
#include "ble.h"
#include "wifi_ntp.h"
#include "logger.h"
#include "display.h"

// ── Hardware Configuration (ESP32-C6) ────────────────────────────────────────
// I2C — all sensors share one bus (Wire, master)
#define SDA_PIN  6   // XIAO ESP32-C6 SDA
#define SCL_PIN  7   // XIAO ESP32-C6 SCL

// MMA8451 I2C addresses (set by SA0 pin)
#define MMA_ADDR_0  0x1C   // SA0 = GND
#define MMA_ADDR_1  0x1D   // SA0 = VCC

// LDC1612 I2C address (ADDR pin sets bit 0: 0x2A or 0x2B)
#define LDC_ADDR    DEFAULT_IIC_ADDR  // 0x2B

// ADC
#define ADC_PIN  1   // GPIO1 = ADC1_CH1 — ECG

// Sample rates
#define SAMPLE_RATE_HZ         100   // ECG
#define ACCEL_RATE_HZ           50   // MMA8451 + LDC1612 + SDP800
#define BLE_RESCAN_INTERVAL_MS 10000
#define MAX_SCAN_RETRIES        10
#define SLEEP_DURATION_US       (10ULL * 60ULL * 1000000ULL)

// ── Sensor objects ────────────────────────────────────────────────────────────
Adafruit_MMA8451 mma0;
Adafruit_MMA8451 mma1;
LDC1612          ldc(LDC_ADDR);
Sdp800           sdp800;

// ── Sleep State ───────────────────────────────────────────────────────────────
RTC_DATA_ATTR bool wokeFromSleep = false;

// ── Sensor Data ───────────────────────────────────────────────────────────────
volatile uint16_t latestAdcValue = 0;
volatile int16_t  latestAccel0X  = 0, latestAccel0Y = 0, latestAccel0Z = 0;
volatile int16_t  latestAccel1X  = 0, latestAccel1Y = 0, latestAccel1Z = 0;
volatile uint32_t latestLdc0     = 0;
volatile uint32_t latestLdc1     = 0;
volatile float    latestPressure = 0.0f;

unsigned long lastEcgSampleTime   = 0;
unsigned long lastAccelSampleTime = 0;
unsigned long lastScanTime        = 0;
unsigned long lastLogMillis       = 0;
unsigned long lastPrintMillis     = 0;
unsigned long lastDisplayMillis   = 0;

const unsigned long ecgInterval   = 1000 / SAMPLE_RATE_HZ;  // 10 ms
const unsigned long accelInterval = 1000 / ACCEL_RATE_HZ;   // 20 ms

static void enterDeepSleep() {
    Serial.println("Entering deep sleep for 10 minutes...");
    Serial.flush();
    closeLogger();
    wokeFromSleep = true;
    esp_sleep_enable_timer_wakeup(SLEEP_DURATION_US);
    esp_deep_sleep_start();
}

void setup() {
    setCpuFrequencyMhz(80);
    Serial.begin(115200);
    initDisplay();

    Wire.begin(SDA_PIN, SCL_PIN);

    // MMA8451 accelerometers
    if (!mma0.begin(MMA_ADDR_0))
        Serial.println("MMA8451 #0 (0x1C) not found");
    else {
        mma0.setRange(MMA8451_RANGE_2_G);
        mma0.setDataRate(MMA8451_DATARATE_50_HZ);
    }
    if (!mma1.begin(MMA_ADDR_1))
        Serial.println("MMA8451 #1 (0x1D) not found");
    else {
        mma1.setRange(MMA8451_RANGE_2_G);
        mma1.setDataRate(MMA8451_DATARATE_50_HZ);
    }

    // LDC1612 inductive sensor
    if (!ldc.begin(&Wire)) {
        Serial.println("LDC1612 not found");
    } else {
        ldc.read_sensor_information();
        ldc.reset_sensor();
        ldc.LDC1612_mutiple_channel_config();
    }

    // SDP800 differential pressure sensor
    if (!sdp800.begin(&Wire))
        Serial.println("SDP800 not found");

    // ADC — 12-bit (0–4095)
    analogReadResolution(12);

    syncTimeFromNTP();
    initLogger();

    BLEDevice::init("ESP32_HRM_Client");
    if (wokeFromSleep)
        Serial.println("Woke from deep sleep. Single reconnect attempt.");
    startBLEScan();

    Serial.println("System initialized at 80 MHz");
}

void loop() {
    unsigned long currentTime = millis();

    // ── 100 Hz ECG ────────────────────────────────────────────────────────────
    if (currentTime - lastEcgSampleTime >= ecgInterval) {
        lastEcgSampleTime = currentTime;
        latestAdcValue    = analogRead(ADC_PIN);
    }

    // ── 50 Hz: accel, inductance, pressure ───────────────────────────────────
    if (currentTime - lastAccelSampleTime >= accelInterval) {
        lastAccelSampleTime = currentTime;

        mma0.read();
        latestAccel0X = mma0.x;  latestAccel0Y = mma0.y;  latestAccel0Z = mma0.z;

        mma1.read();
        latestAccel1X = mma1.x;  latestAccel1Y = mma1.y;  latestAccel1Z = mma1.z;

        uint32_t ch0 = 0, ch1 = 0;
        ldc.get_channel_result(CHANNEL_0, &ch0);
        ldc.get_channel_result(CHANNEL_1, &ch1);
        latestLdc0 = ch0;
        latestLdc1 = ch1;

        float p = 0.0f;
        sdp800.readPressureMbar(p);
        latestPressure = p;
    }

    // ── 1 Hz console print ────────────────────────────────────────────────────
    if (!dumping && currentTime - lastPrintMillis >= 1000) {
        lastPrintMillis = currentTime;
        Serial.printf("A0: %d %d %d  A1: %d %d %d  LDC: %lu %lu  P: %.4f mbar\n",
                      latestAccel0X, latestAccel0Y, latestAccel0Z,
                      latestAccel1X, latestAccel1Y, latestAccel1Z,
                      latestLdc0, latestLdc1, latestPressure);
    }

    // ── BLE connection management ─────────────────────────────────────────────
    if (doConnect && !bleConnecting) {
        doConnect     = false;
        bleConnecting = true;
        xTaskCreate(bleConnectTask, "BLEConnect", 4096, nullptr, 1, nullptr);
    }

    if (!connected && !doConnect && !bleConnecting &&
        currentTime - lastScanTime >= BLE_RESCAN_INTERVAL_MS) {
        int maxAttempts = wokeFromSleep ? 1 : MAX_SCAN_RETRIES;
        if (scanAttempts >= maxAttempts) {
            enterDeepSleep();
        } else {
            startBLEScan();
        }
    }

    // ── 50 Hz logging ─────────────────────────────────────────────────────────
    if (currentTime - lastLogMillis >= LOG_RATE_MS) {
        lastLogMillis = currentTime;
        logRecord(latestAccel0X, latestAccel0Y, latestAccel0Z,
                  latestAccel1X, latestAccel1Y, latestAccel1Z,
                  latestLdc0, latestLdc1,
                  latestAdcValue, latestPressure,
                  latestRR_ms);
    }

    // ── 5 Hz display update ───────────────────────────────────────────────────
    if (currentTime - lastDisplayMillis >= DISPLAY_RATE_MS) {
        lastDisplayMillis = currentTime;
        updateDisplay();
    }

    // ── Serial commands ───────────────────────────────────────────────────────
    if (Serial.available()) {
        processLogCommand(Serial.read());
    }

    delay(1);
}
