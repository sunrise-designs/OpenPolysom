#pragma once
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// State written by BLE callbacks / connect task, read in the main loop
extern volatile boolean  doConnect;
extern volatile boolean  connected;
extern volatile boolean  bleConnecting;
extern volatile uint16_t latestRR_ms;
extern volatile uint16_t latestBpm;
extern int               scanAttempts;

// Set from the scan result; empty until first device found
extern String bleDeviceName;
extern String bleDeviceAddress;

// Shared timing — set by startBLEScan(), checked in loop() for rescan
extern unsigned long lastScanTime;

// RTC-backed flag — reset to false on successful connect (defined in main .ino)
extern bool wokeFromSleep;

void startBLEScan();
void bleConnectTask(void* pvParameters);
