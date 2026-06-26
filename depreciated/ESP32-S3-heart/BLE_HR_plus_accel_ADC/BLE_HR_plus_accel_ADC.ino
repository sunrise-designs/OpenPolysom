#include "Wire.h"
#include "esp_sleep.h"
#include "ble.h"
#include "wifi_ntp.h"
#include "logger.h"

// --- Hardware Configuration ---
#define I2C_SLAVE_ADDR         0x30
#define SDA_PIN                14
#define SCL_PIN                21
#define ADC_PIN                1   // GPIO1 = ADC1_CH0 — ECG
#define ACCEL_PIN_X            2   // GPIO2 = ADC1_CH1 — ADXL335 X
#define ACCEL_PIN_Y            3   // GPIO3 = ADC1_CH2 — ADXL335 Y
#define ACCEL_PIN_Z            4   // GPIO4 = ADC1_CH3 — ADXL335 Z
#define SAMPLE_RATE_HZ         100
#define ACCEL_RATE_HZ          50
#define BLE_RESCAN_INTERVAL_MS 10000
#define MAX_SCAN_RETRIES       10
#define SLEEP_DURATION_US      (10ULL * 60ULL * 1000000ULL)  // 10 minutes

// --- Sleep State (survives deep sleep via RTC memory) ---
RTC_DATA_ATTR bool wokeFromSleep = false;

// --- Sensor Data ---
volatile uint16_t latestAdcValue = 0;
volatile uint16_t latestAccelX   = 0;
volatile uint16_t latestAccelY   = 0;
volatile uint16_t latestAccelZ   = 0;

unsigned long lastEcgSampleTime   = 0;
unsigned long lastAccelSampleTime = 0;
unsigned long lastScanTime        = 0;
unsigned long lastLogMillis       = 0;
unsigned long lastPrintMillis     = 0;

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

// Forward declaration so Wire.onRequest() can reference it before definition.
void onRequest();

void setup() {
  // BLE requires at least 80MHz; 40MHz caused instability with the radio stack
  setCpuFrequencyMhz(80);

  Serial.begin(115200);

  // I2C Slave
  if (!Wire.begin(I2C_SLAVE_ADDR, SDA_PIN, SCL_PIN, 100000)) {
    Serial.println("I2C Init Failed");
    while (1);
  }
  Wire.onRequest(onRequest);

  // ADC — 12-bit (0–4095)
  analogReadResolution(12);

  // NTP sync before LittleFS so the header timestamp is valid; WiFi off before BLE.
  syncTimeFromNTP();

  initLogger();

  // BLE
  BLEDevice::init("ESP32_HRM_Client");

  if (wokeFromSleep)
    Serial.println("Woke from deep sleep. Single reconnect attempt.");

  startBLEScan();

  Serial.println("System Initialized at 80MHz");
}

void loop() {
  unsigned long currentTime = millis();

  // --- Non-blocking 100 Hz ECG Sampling ---
  if (currentTime - lastEcgSampleTime >= ecgInterval) {
    lastEcgSampleTime = currentTime;
    latestAdcValue = analogRead(ADC_PIN);
  }

  // --- Non-blocking 50 Hz Accelerometer Sampling ---
  if (currentTime - lastAccelSampleTime >= accelInterval) {
    lastAccelSampleTime = currentTime;
    latestAccelX = analogRead(ACCEL_PIN_X);
    latestAccelY = analogRead(ACCEL_PIN_Y);
    latestAccelZ = analogRead(ACCEL_PIN_Z);
  }

  // --- 1 Hz Console Print ---
  if (!dumping && currentTime - lastPrintMillis >= 1000) {
    lastPrintMillis = currentTime;
    Serial.printf("X: %d  Y: %d  Z: %d\n", latestAccelX, latestAccelY, latestAccelZ);
  }

  // --- BLE Connection Management ---
  // Spawn a task so pClient->connect() (blocks up to ~10 s) never stalls the loop.
  if (doConnect && !bleConnecting) {
    doConnect     = false;
    bleConnecting = true;
    xTaskCreate(bleConnectTask, "BLEConnect", 4096, nullptr, 1, nullptr);
  }

  // Rescan if not connected and no connection attempt is in progress.
  if (!connected && !doConnect && !bleConnecting &&
      currentTime - lastScanTime >= BLE_RESCAN_INTERVAL_MS) {
    int maxAttempts = wokeFromSleep ? 1 : MAX_SCAN_RETRIES;
    if (scanAttempts >= maxAttempts) {
      enterDeepSleep();
    } else {
      startBLEScan();
    }
  }

  // --- 10 Hz Flash Logging ---
  if (currentTime - lastLogMillis >= LOG_RATE_MS) {
    lastLogMillis = currentTime;
    logRecord(latestAccelX, latestAccelY, latestAccelZ, latestRR_ms);
  }

  // --- Serial Commands ---
  if (Serial.available()) {
    processLogCommand(Serial.read());
  }

  // Yield to RTOS tasks (BLE stack, IDLE)
  delay(1);
}

// --- I2C Request Handler ---
// ISR context. Sends 10 bytes: [ECG_H, ECG_L, X_H, X_L, Y_H, Y_L, Z_H, Z_L, RR_H, RR_L]
void onRequest() {
  uint8_t buffer[10];

  buffer[0] = (latestAdcValue >> 8) & 0xFF;
  buffer[1] =  latestAdcValue       & 0xFF;

  buffer[2] = (latestAccelX >> 8) & 0xFF;
  buffer[3] =  latestAccelX       & 0xFF;

  buffer[4] = (latestAccelY >> 8) & 0xFF;
  buffer[5] =  latestAccelY       & 0xFF;

  buffer[6] = (latestAccelZ >> 8) & 0xFF;
  buffer[7] =  latestAccelZ       & 0xFF;

  buffer[8] = (latestRR_ms >> 8) & 0xFF;
  buffer[9] =  latestRR_ms       & 0xFF;

  Wire.write(buffer, 10);
}
