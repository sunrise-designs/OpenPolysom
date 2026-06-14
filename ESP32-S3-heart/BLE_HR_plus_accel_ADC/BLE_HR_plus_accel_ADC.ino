#include "Wire.h"
#include <WiFi.h>
#include <math.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

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
#define BLE_RESCAN_INTERVAL_MS 10000

// --- BLE UUIDs (Bluetooth SIG standard) ---
static BLEUUID serviceUUID((uint16_t)0x180D);  // Heart Rate Service
static BLEUUID charUUID((uint16_t)0x2A37);     // Heart Rate Measurement

// --- BLE State ---
// Written from BLE scan task / BLE connect task, read in main loop — must be volatile
static volatile boolean doConnect     = false;
static volatile boolean connected     = false;
static volatile boolean bleConnecting = false;  // true while connect task is running
static BLEAdvertisedDevice* myDevice  = nullptr;
static BLEClient*           pClient   = nullptr;

// --- Sensor Data ---
volatile uint16_t latestAdcValue  = 0;
// Magnitude of raw 12-bit X/Y/Z: max sqrt(4095^2 * 3) ~= 7092, fits uint16_t
volatile uint16_t latestMagnitude = 0;
// RR interval in ms (BLE 1/1024 s units → ms). Typical range 300–2000 ms.
volatile uint16_t latestRR_ms     = 0;

unsigned long lastEcgSampleTime   = 0;
unsigned long lastAccelSampleTime = 0;
unsigned long lastScanTime        = 0;

const unsigned long ecgInterval   = 1000 / SAMPLE_RATE_HZ; // 10ms
const unsigned long accelInterval = 1000 / ACCEL_RATE_HZ;  // 20ms

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
    Serial.printf("BPM: %d\n", bpm);
    offset = 3;
  } else {
    Serial.printf("BPM: %d\n", pData[1]);
    offset = 2;
  }

  if (eePresent) offset += 2;

  if (rrPresent) {
    // A single BLE packet may carry multiple RR values; keep the last one.
    while (offset + 1 < (int)length) {
      uint16_t rr_raw = (uint16_t)pData[offset] | ((uint16_t)pData[offset + 1] << 8);
      // BT SIG: RR unit is 1/1024 s
      uint16_t rr_ms = (uint16_t)((rr_raw * 1000UL) / 1024UL);
      Serial.printf("RR Interval: %d ms\n", rr_ms);
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
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);
  pBLEScan->setActiveScan(true);
  Serial.println("Scanning for Heart Rate sensor...");
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
  if (!Wire.begin(I2C_SLAVE_ADDR, SDA_PIN, SCL_PIN, 0)) {
    Serial.println("I2C Init Failed");
    while (1);
  }
  Wire.onRequest(onRequest);

  // ADC — 12-bit (0–4095)
  analogReadResolution(12);

  // BLE
  BLEDevice::init("ESP32_HRM_Client");
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

    uint16_t x = analogRead(ACCEL_PIN_X);
    uint16_t y = analogRead(ACCEL_PIN_Y);
    uint16_t z = analogRead(ACCEL_PIN_Z);

    // Raw magnitude — no offset removal, no normalisation
    float mag = sqrtf((float)x * x + (float)y * y + (float)z * z);
    latestMagnitude = (uint16_t)mag;
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
    startBLEScan();
  }

  // Yield to RTOS tasks (BLE stack, IDLE)
  delay(1);
}

// --- I2C Request Handler ---
// ISR context. Sends 6 bytes: [ECG_H, ECG_L, MAG_H, MAG_L, RR_H, RR_L]
void onRequest() {
  uint8_t buffer[6];

  buffer[0] = (latestAdcValue  >> 8) & 0xFF;
  buffer[1] =  latestAdcValue        & 0xFF;

  buffer[2] = (latestMagnitude >> 8) & 0xFF;
  buffer[3] =  latestMagnitude       & 0xFF;

  buffer[4] = (latestRR_ms    >> 8) & 0xFF;
  buffer[5] =  latestRR_ms          & 0xFF;

  Wire.write(buffer, 6);
}
