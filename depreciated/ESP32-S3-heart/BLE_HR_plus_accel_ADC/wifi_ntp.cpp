#include "wifi_ntp.h"
#include <WiFi.h>
#include <Arduino.h>

#define WIFI_SSID        "TNCAP26203D"
#define WIFI_PASSWORD    "pFynYNnaMNk9syGR"
#define NTP_SERVER1      "pool.ntp.org"
#define NTP_SERVER2      "time.nist.gov"
#define WIFI_TIMEOUT_MS  10000
#define NTP_TIMEOUT_MS    8000

void syncTimeFromNTP() {
  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi: connection failed — skipping NTP sync");
    WiFi.mode(WIFI_OFF);
    return;
  }

  Serial.println("WiFi connected. Syncing time...");
  configTime(0, 0, NTP_SERVER1, NTP_SERVER2);  // UTC

  struct tm timeInfo;
  t0 = millis();
  while (!getLocalTime(&timeInfo) && millis() - t0 < NTP_TIMEOUT_MS) {
    delay(200);
  }

  if (!getLocalTime(&timeInfo)) {
    Serial.println("NTP: time sync failed");
  } else {
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &timeInfo);
    Serial.printf("Current time: %s\n", buf);
  }

  WiFi.mode(WIFI_OFF);
}
