#pragma once

#include "hal/gpio_types.h"

// ── I2C ───────────────────────────────────────────────────────────────────────
// All sensors share one I2C bus.
#define I2C_PORT      I2C_NUM_0
#define I2C_SDA_PIN   ((gpio_num_t)6)    // PLACEHOLDER — match board schematic
#define I2C_SCL_PIN   ((gpio_num_t)7)    // PLACEHOLDER
#define I2C_FREQ_HZ   400000

// ── MMA8451 accelerometer I2C addresses ──────────────────────────────────────
#define MMA_ADDR_0    0x1C  // SA0 = GND
#define MMA_ADDR_1    0x1D  // SA0 = VCC

// ── LDC1612 inductive sensor ──────────────────────────────────────────────────
#define LDC_ADDR      0x2B  // DEFAULT_IIC_ADDR

// ── SDP800 differential pressure sensor ──────────────────────────────────────
#define SDP800_ADDR   0x25

// ── ADC — ECG signal ──────────────────────────────────────────────────────────
#define ECG_ADC_UNIT    ADC_UNIT_1
#define ECG_ADC_CHANNEL ADC_CHANNEL_1   // GPIO1

// ── Shared SPI bus (LCD + SD card) ───────────────────────────────────────────
#define SPI_HOST_ID   SPI2_HOST
#define SPI_MOSI_PIN  ((gpio_num_t)6)    // GPIO6 — matches reference board
#define SPI_MISO_PIN  ((gpio_num_t)12)   // needed by SD, ignored by LCD
#define SPI_CLK_PIN   ((gpio_num_t)7)    // GPIO7 — matches reference board

// ── ST7789 LCD (172×320) ──────────────────────────────────────────────────────
#define LCD_CS_PIN    ((gpio_num_t)14)
#define LCD_DC_PIN    ((gpio_num_t)15)
#define LCD_RST_PIN   ((gpio_num_t)21)
#define LCD_BL_PIN    ((gpio_num_t)22)   // backlight (active HIGH)
#define LCD_W         172
#define LCD_H         320
#define LCD_SPI_FREQ  12000000  // 12 MHz — matches reference board

// ── SD card ───────────────────────────────────────────────────────────────────
#define SD_CS_PIN     ((gpio_num_t)10)   // PLACEHOLDER
#define SD_SPI_FREQ   20000000  // 20 MHz

// ── Sample rates ──────────────────────────────────────────────────────────────
#define ECG_RATE_HZ           100
#define SENSOR_RATE_HZ         50
#define DISPLAY_RATE_HZ         5
#define LOG_RATE_MS            20   // 50 Hz
#define DISPLAY_RATE_MS       200   // 5 Hz
#define BLE_RESCAN_MS        10000
#define MAX_SCAN_RETRIES       10
#define SLEEP_DURATION_US  (10ULL * 60ULL * 1000000ULL)

// ── Wi-Fi / NTP (fallbacks; Kconfig values take precedence via sdkconfig.h) ──
#ifndef CONFIG_POLYSOM_WIFI_SSID
#define CONFIG_POLYSOM_WIFI_SSID      "TNCAP26203D"
#endif
#ifndef CONFIG_POLYSOM_WIFI_PASSWORD
#define CONFIG_POLYSOM_WIFI_PASSWORD  ""
#endif
#ifndef CONFIG_POLYSOM_NTP_SERVER
#define CONFIG_POLYSOM_NTP_SERVER     "pool.ntp.org"
#endif

// ── EDF file ──────────────────────────────────────────────────────────────────
#define EDF_FILE_PATH   "/sdcard/biometric.edf"
#define RECORD_DURATION_S  10
#define SAMPLES_50HZ      500   // 50 Hz × 10 s
#define SAMPLES_1HZ        10   // 1 Hz × 10 s
