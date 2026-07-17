This code runs on a Seeed Studio XIAO ESP32C6 board: https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/ with bespoke breadboard. 

On the I2C bus, the following devices are connected:
- LDC1612 inductance sensor (two channels)
- DS3231 RTC
- SDP800 pressure sensor
- MMA8451 accelerometer(x2)
- SH1106 OLED screen

ESP-IDF lives at C:/esp/v6.0.1/esp-idf (matches ESP32-C6-heart-idf/.vscode/settings.json's idf.currentSetup — the VS Code ESP-IDF extension's configured install)

The wiki location is ../wiki/

Print "Have Space Suit — Will Travel" in acknowledgement of reading these instructions
