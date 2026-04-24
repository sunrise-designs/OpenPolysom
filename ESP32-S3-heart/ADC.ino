#include "Wire.h"
#include <WiFi.h>

// --- Configuration ---
#define I2C_SLAVE_ADDR 0x30
#define SDA_PIN 14
#define SCL_PIN 21
#define ADC_PIN 1      // GPIO 1 is ADC1_CH0 on ESP32-S3
#define SAMPLE_RATE_HZ 100

// --- Global Variables ---
volatile uint16_t latestAdcValue = 0;
unsigned long lastSampleTime = 0;
const unsigned long sampleInterval = 1000 / SAMPLE_RATE_HZ; // 10ms

void setup() {
  // 1. Set CPU Frequency to 40MHz for power saving
  // Note: 40MHz is generally the lowest stable freq for I2C/Serial on S3
  setCpuFrequencyMhz(40);

  // 2. Disable Radio Peripherals (WiFi and Bluetooth)
  WiFi.mode(WIFI_OFF);
  btStop();

  // 3. Initialize Serial for debugging (Optional)
  Serial.begin(115200);

  // 4. Initialize I2C Slave
  // Wire.begin(address, SDA, SCL, frequency)
  if (!Wire.begin(I2C_SLAVE_ADDR, SDA_PIN, SCL_PIN, 0)) {
    Serial.println("I2C Init Failed");
    while (1);
  }

  // 5. Register I2C Request Callback
  Wire.onRequest(onRequest);

  // 6. Configure ADC
  // ESP32-S3 ADC is 12-bit by default (0-4095)
  analogReadResolution(12);
  
  Serial.println("System Initialized at 40MHz");
}

void loop() {
  // --- Non-blocking 100Hz Sampling ---
  unsigned long currentTime = millis();
  
  if (currentTime - lastSampleTime >= sampleInterval) {
    lastSampleTime = currentTime;
    
    // Sample ADC Channel 0
    // We store it in a volatile variable for thread-safety with the I2C interrupt
    latestAdcValue = analogRead(ADC_PIN);
  }

  // The CPU stays in a light "Idle" state between loop iterations
  // yielding to the RTOS IDLE task to save power.
  delay(1); 
}

// --- I2C Request Handler ---
// This runs in an ISR context when the Master requests data
void onRequest() {
  uint8_t buffer[2];
  
  // Split 12-bit ADC value into two bytes (Big Endian)
  buffer[0] = (latestAdcValue >> 8) & 0xFF; // High byte
  buffer[1] = latestAdcValue & 0xFF;        // Low byte
  
  // Write exactly 2 bytes as requested by the Master
  Wire.write(buffer, 2);
}