#include "Wire.h"
#include <WiFi.h>
#include <math.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include "esp_sleep.h"
#include <LittleFS.h>

// --- Configuration ---
#define I2C_SLAVE_ADDR 0x30
#define SDA_PIN 14
#define SCL_PIN 21
#define ADC_PIN 1          // GPIO1 = ADC1_CH0 — ECG
#define ACCEL_PIN_X 2      // GPIO2 = ADC1_CH1 — ADXL335 X
#define ACCEL_PIN_Y 3      // GPIO3 = ADC1_CH2 — ADXL335 Y
#define ACCEL_PIN_Z 4      // GPIO4 = ADC1_CH3 — ADXL335 Z
#define SAMPLE_RATE_HZ 100
#define ACCEL_RATE_HZ  50
#define BLE_RESCAN_INTERVAL_MS  10000
#define MAX_SCAN_RETRIES        10
#define SLEEP_DURATION_US       (10ULL * 60ULL * 1000000ULL)  // 10 minutes

// --- Flash Logging ---
// Record layout (5 bytes): [X_8, Y_8, Z_8, RR_L, RR_H]
// Accel axes scaled 12-bit → 8-bit (>> 4). RR in ms, little-endian uint16_t.
// 6 h × 10 Hz × 5 bytes = 1,080,000 bytes (~1.03 MB). Fits in default 1.5 MB LittleFS.
#define LOG_FILE           "/biometric.bin"
#define LOG_RATE_MS        100      // 10 Hz
#define MAX_RECORDS        216000UL // 6 hours at 10 Hz
#define FLUSH_EVERY        600      // flush to flash every 60 s (600 records × 100 ms)

// --- BLE UUIDs (Bluetooth SIG standard) ---
static BLEUUID serviceUUID((uint16_t)0x180D);  // Heart Rate Service
static BLEUUID charUUID((uint16_t)0x2A37);     // Heart Rate Measurement

// --- Sleep State (survives deep sleep via RTC memory) ---
RTC_DATA_ATTR bool wokeFromSleep = false;

// --- BLE State ---
// Written from BLE scan task / BLE connect task, read in main loop — must be volatile
static volatile boolean doConnect     = false;
static volatile boolean connected     = false;
static volatile boolean bleConnecting = false;  // true while connect task is running
static BLEAdvertisedDevice* myDevice  = nullptr;
static BLEClient*           pClient   = nullptr;

static int scanAttempts = 0;

// --- Sensor Data ---
volatile uint16_t latestAdcValue  = 0;
volatile uint16_t latestAccelX    = 0;
volatile uint16_t latestAccelY    = 0;
volatile uint16_t latestAccelZ    = 0;
// RR interval in ms (BLE 1/1024 s units → ms). Typical range 300–2000 ms.
volatile uint16_t latestRR_ms     = 0;

unsigned long lastEcgSampleTime   = 0;
unsigned long lastAccelSampleTime = 0;
unsigned long lastScanTime        = 0;
unsigned long lastLogMillis       = 0;
unsigned long lastPrintMillis     = 0;

static File             logFile;
static uint32_t         recordCount   = 0;
static bool             loggingActive = false;
static volatile bool    dumping       = false;

const unsigned long ecgInterval   = 1000 / SAMPLE_RATE_HZ; // 10ms
const unsigned long accelInterval = 1000 / ACCEL_RATE_HZ;  // 20ms

// --- Deep Sleep ---
static void enterDeepSleep() {
  Serial.println("Entering deep sleep for 10 minutes...");
  Serial.flush();
  if (loggingActive && logFile) {
    logFile.flush();
    logFile.close();
    loggingActive = false;
  }
  wokeFromSleep = true;
  esp_sleep_enable_timer_wakeup(SLEEP_DURATION_US);
  esp_deep_sleep_start();
}

// --- BLE Notification Callback ---
static void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic,
                           uint8_t* pData, size_t length, bool isNotify) {
  if (length < 2) return;

  uint8_t flags     = pData[0];
  bool is16BitBpm   = (flags >> 0) & 0x01;  // Bit 0: BPM format
  bool eePresent    = (flags >> 3) & 0x01;  // Bit 3: Energy Expended present
  bool rrPresent    = (flags >> 4) & 0x01;  // Bit 4: RR-Interval present

  int offset = 1;

  if (is16BitBpm) {
    uint16_t bpm = (uint16_t)pData[1] | ((uint16_t)pData[2] << 8);
    if (!dumping) Serial.printf("BPM: %d\n", bpm);
    offset = 3;
  } else {
    if (!dumping) Serial.printf("BPM: %d\n", pData[1]);
    offset = 2;
  }

  if (eePresent) offset += 2;

  if (rrPresent) {
    // A single BLE packet may carry multiple RR values; keep the last one.
    while (offset + 1 < (int)length) {
      uint16_t rr_raw = (uint16_t)pData[offset] | ((uint16_t)pData[offset + 1] << 8);
      // BT SIG: RR unit is 1/1024 s
      uint16_t rr_ms = (uint16_t)((rr_raw * 1000UL) / 1024UL);
      if (!dumping) Serial.printf("RR Interval: %d ms\n", rr_ms);
      latestRR_ms = rr_ms;
      offset += 2;
    }
  }
}

// --- BLE Client Callbacks ---
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {}
  void onDisconnect(BLEClient* pclient) {
    connected = false;
    Serial.println("Heart Rate Sensor Disconnected.");
  }
};

// --- BLE Connection ---
bool connectToServer() {
  Serial.print("Connecting to: ");
  Serial.println(myDevice->getAddress().toString().c_str());

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());
  pClient->connect(myDevice);

  BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService == nullptr) {
    Serial.println("Failed to find Heart Rate service.");
    pClient->disconnect();
    return false;
  }

  BLERemoteCharacteristic* pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
  if (pRemoteCharacteristic == nullptr) {
    Serial.println("Failed to find Measurement characteristic.");
    pClient->disconnect();
    return false;
  }

  if (pRemoteCharacteristic->canNotify()) {
    pRemoteCharacteristic->registerForNotify(notifyCallback);
  }

  connected = true;
  return true;
}

// --- BLE Connect Task ---
// Runs connectToServer() on a separate RTOS task so pClient->connect() (which
// blocks for up to ~10 s waiting for the radio) never stalls the main loop.
void bleConnectTask(void* pvParameters) {
  if (connectToServer()) {
    Serial.println("Connected. Subscribed to HR notifications.");
    scanAttempts   = 0;
    wokeFromSleep  = false;
  } else {
    Serial.println("Failed to connect. Will retry.");
  }
  bleConnecting = false;
  vTaskDelete(nullptr);  // task must delete itself when done
}

// --- BLE Scan Callbacks ---
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.haveServiceUUID() &&
        advertisedDevice.isAdvertisingService(serviceUUID)) {
      BLEDevice::getScan()->stop();
      myDevice  = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

void startBLEScan() {
  scanAttempts++;
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);
  pBLEScan->setActiveScan(true);
  Serial.printf("Scanning for Heart Rate sensor (attempt %d)...\n", scanAttempts);
  pBLEScan->start(5, false);
  lastScanTime = millis();
}

// ============================================================

void setup() {
  // BLE requires at least 80MHz; 40MHz caused instability with the radio stack
  setCpuFrequencyMhz(80);

  WiFi.mode(WIFI_OFF);
  // btStop() removed — BLE stack must remain running

  Serial.begin(115200);

  // I2C Slave
  if (!Wire.begin(I2C_SLAVE_ADDR, SDA_PIN, SCL_PIN, 100000)) {
    Serial.println("I2C Init Failed");
    while (1);
  }
  Wire.onRequest(onRequest);

  // ADC — 12-bit (0–4095)
  analogReadResolution(12);

  // Flash logging
  if (LittleFS.begin(true)) {
    logFile = LittleFS.open(LOG_FILE, FILE_APPEND);
    if (logFile) {
      recordCount   = logFile.size() / 5;
      loggingActive = (recordCount < MAX_RECORDS);
      Serial.printf("LittleFS: %lu records already stored\n", recordCount);
    } else {
      Serial.println("LittleFS: failed to open log file");
    }
  } else {
    Serial.println("LittleFS: mount failed");
  }

  // BLE
  BLEDevice::init("ESP32_HRM_Client");

  if (wokeFromSleep) {
    Serial.println("Woke from deep sleep. Single reconnect attempt.");
  }

  startBLEScan();

  Serial.println("System Initialized at 80MHz");
}

void loop() {
  unsigned long currentTime = millis();

  // --- Non-blocking 100Hz ECG Sampling ---
  if (currentTime - lastEcgSampleTime >= ecgInterval) {
    lastEcgSampleTime = currentTime;
    latestAdcValue = analogRead(ADC_PIN);
  }

  // --- Non-blocking 50Hz Accelerometer Sampling ---
  if (currentTime - lastAccelSampleTime >= accelInterval) {
    lastAccelSampleTime = currentTime;

    latestAccelX = analogRead(ACCEL_PIN_X);
    latestAccelY = analogRead(ACCEL_PIN_Y);
    latestAccelZ = analogRead(ACCEL_PIN_Z);
  }

  // --- 1 Hz ADC print ---
  if (!dumping && (currentTime - lastPrintMillis >= 1000)) {
    lastPrintMillis = currentTime;
    Serial.printf("X: %d  Y: %d  Z: %d\n", latestAccelX, latestAccelY, latestAccelZ);
  }

  // --- BLE Connection Management ---
  // Spawn a task so pClient->connect() never blocks the main loop.
  if (doConnect && !bleConnecting) {
    doConnect     = false;
    bleConnecting = true;
    xTaskCreate(bleConnectTask, "BLEConnect", 4096, nullptr, 1, nullptr);
  }

  // Rescan if not connected and no connection attempt is in progress
  if (!connected && !doConnect && !bleConnecting &&
      (currentTime - lastScanTime >= BLE_RESCAN_INTERVAL_MS)) {
    int maxAttempts = wokeFromSleep ? 1 : MAX_SCAN_RETRIES;
    if (scanAttempts >= maxAttempts) {
      enterDeepSleep();
    } else {
      startBLEScan();
    }
  }

  // --- 10 Hz Flash Logging ---
  if (loggingActive && (currentTime - lastLogMillis >= LOG_RATE_MS)) {
    lastLogMillis = currentTime;

    uint8_t rec[5];
    rec[0] = (uint8_t)(latestAccelX >> 4);
    rec[1] = (uint8_t)(latestAccelY >> 4);
    rec[2] = (uint8_t)(latestAccelZ >> 4);
    rec[3] = (uint8_t)(latestRR_ms & 0xFF);
    rec[4] = (uint8_t)(latestRR_ms >> 8);
    logFile.write(rec, 5);

    recordCount++;
    if (recordCount % FLUSH_EVERY == 0) {
      logFile.flush();
    }
    if (recordCount >= MAX_RECORDS) {
      logFile.flush();
      logFile.close();
      loggingActive = false;
      Serial.println("Flash log full (6 hours).");
    }
  }

  // --- Serial dump command ('D' + Enter → streams full log as hex) ---
  if (Serial.available()) {
    char cmd = Serial.read();
    if (cmd == 'E') {
      if (loggingActive && logFile) {
        logFile.close();
        loggingActive = false;
      }
      LittleFS.remove(LOG_FILE);
      logFile = LittleFS.open(LOG_FILE, FILE_APPEND);
      if (logFile) {
        recordCount   = 0;
        loggingActive = true;
        Serial.println("Log erased. Recording restarted.");
      } else {
        Serial.println("Log erase failed.");
      }
    } else if (cmd == 'D') {
      dumping = true;
      if (loggingActive && logFile) {
        logFile.flush();
        logFile.close();
        loggingActive = false;
      }
      File rf = LittleFS.open(LOG_FILE, FILE_READ);
      Serial.printf("DUMP_START:%lu\n", rf ? rf.size() : 0UL);
      if (rf) {
        uint8_t buf[64];
        int n;
        while ((n = rf.read(buf, sizeof(buf))) > 0) {
          for (int i = 0; i < n; i++) Serial.printf("%02X", buf[i]);
        }
        rf.close();
      }
      Serial.print("\nDUMP_END\n");
      dumping = false;
      // Reopen for append
      logFile = LittleFS.open(LOG_FILE, FILE_APPEND);
      if (logFile) {
        recordCount   = logFile.size() / 5;
        loggingActive = (recordCount < MAX_RECORDS);
      }
    }
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
