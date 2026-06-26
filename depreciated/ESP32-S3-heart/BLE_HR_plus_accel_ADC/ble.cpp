#include "ble.h"
#include "logger.h"  // for dumping flag
#include <Arduino.h>

static BLEUUID serviceUUID((uint16_t)0x180D);  // Heart Rate Service
static BLEUUID charUUID((uint16_t)0x2A37);     // Heart Rate Measurement

volatile boolean  doConnect     = false;
volatile boolean  connected     = false;
volatile boolean  bleConnecting = false;
volatile uint16_t latestRR_ms  = 0;
int               scanAttempts  = 0;

static BLEAdvertisedDevice* myDevice = nullptr;
static BLEClient*           pClient  = nullptr;

static void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic,
                           uint8_t* pData, size_t length, bool isNotify) {
  if (length < 2) return;

  uint8_t flags   = pData[0];
  bool is16BitBpm = (flags >> 0) & 0x01;  // Bit 0: BPM format
  bool eePresent  = (flags >> 3) & 0x01;  // Bit 3: Energy Expended present
  bool rrPresent  = (flags >> 4) & 0x01;  // Bit 4: RR-Interval present

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

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {}
  void onDisconnect(BLEClient* pclient) {
    connected = false;
    Serial.println("Heart Rate Sensor Disconnected.");
  }
};

static bool connectToServer() {
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

  BLERemoteCharacteristic* pRemoteCharacteristic =
      pRemoteService->getCharacteristic(charUUID);
  if (pRemoteCharacteristic == nullptr) {
    Serial.println("Failed to find Measurement characteristic.");
    pClient->disconnect();
    return false;
  }

  if (pRemoteCharacteristic->canNotify())
    pRemoteCharacteristic->registerForNotify(notifyCallback);

  connected = true;
  return true;
}

// Runs connectToServer() on a separate RTOS task so pClient->connect() (which
// blocks for up to ~10 s) never stalls the main loop.
void bleConnectTask(void* pvParameters) {
  if (connectToServer()) {
    Serial.println("Connected. Subscribed to HR notifications.");
    scanAttempts  = 0;
    wokeFromSleep = false;
  } else {
    Serial.println("Failed to connect. Will retry.");
  }
  bleConnecting = false;
  vTaskDelete(nullptr);  // task must delete itself when done
}

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
