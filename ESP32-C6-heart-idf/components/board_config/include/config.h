#pragma once
#include "sdkconfig.h"
#include "hal/gpio_types.h"
#include <stdint.h>

// ── I2C ───────────────────────────────────────────────────────────────────────
#define I2C_PORT      I2C_NUM_0
#define I2C_SDA_PIN   ((gpio_num_t)CONFIG_POLYSOM_I2C_SDA_PIN)
#define I2C_SCL_PIN   ((gpio_num_t)CONFIG_POLYSOM_I2C_SCL_PIN)
#define I2C_FREQ_HZ   CONFIG_POLYSOM_I2C_FREQ_HZ

// ── MMA8451 accelerometer I2C addresses ──────────────────────────────────────
#define MMA_ADDR_0    CONFIG_POLYSOM_MMA_ADDR_0
#define MMA_ADDR_1    CONFIG_POLYSOM_MMA_ADDR_1

// ── LDC1612 inductive sensor ──────────────────────────────────────────────────
#define LDC_ADDR      CONFIG_POLYSOM_LDC_ADDR

// ── SDP800 differential pressure sensor ──────────────────────────────────────
#define SDP800_ADDR   CONFIG_POLYSOM_SDP800_ADDR

// ── DS1307 real-time clock ────────────────────────────────────────────────────
#define DS1307_ADDR           CONFIG_POLYSOM_DS1307_ADDR
#define RTC_SYNC_INTERVAL_MS  CONFIG_POLYSOM_RTC_SYNC_INTERVAL_MS

// ── ADC — ECG signal (AD8232, raw, logged for later analysis) ────────────────
// ECG_ADC_UNIT stays as the enum constant; ECG_ADC_CHANNEL is the raw integer
// from Kconfig — callers must cast to adc_channel_t at the call site.
#define ECG_ADC_UNIT    ADC_UNIT_1
#define ECG_ADC_CHANNEL CONFIG_POLYSOM_ECG_ADC_CHANNEL

// ── SH1106 OLED (128×64, I2C — shares the sensor I2C bus above) ──────────────
#define OLED_ADDR     CONFIG_POLYSOM_OLED_ADDR
#define OLED_W        CONFIG_POLYSOM_OLED_W
#define OLED_H        CONFIG_POLYSOM_OLED_H

// ── SPI bus (SD card only — the LCD used to share this bus, now I2C above) ──
#define SPI_HOST_ID   SPI2_HOST
#define SPI_MOSI_PIN  ((gpio_num_t)CONFIG_POLYSOM_SPI_MOSI_PIN)
#define SPI_MISO_PIN  ((gpio_num_t)CONFIG_POLYSOM_SPI_MISO_PIN)
#define SPI_CLK_PIN   ((gpio_num_t)CONFIG_POLYSOM_SPI_CLK_PIN)

// ── SD card ───────────────────────────────────────────────────────────────────
#define SD_CS_PIN     ((gpio_num_t)CONFIG_POLYSOM_SD_CS_PIN)
#define SD_SPI_FREQ   CONFIG_POLYSOM_SD_SPI_FREQ_HZ

// ── Sample rates ──────────────────────────────────────────────────────────────
#define ECG_RATE_HZ        CONFIG_POLYSOM_ECG_RATE_HZ
#define SENSOR_RATE_HZ     CONFIG_POLYSOM_SENSOR_RATE_HZ
#define DISPLAY_RATE_HZ    CONFIG_POLYSOM_DISPLAY_RATE_HZ
#define LOG_RATE_MS        CONFIG_POLYSOM_LOG_RATE_MS
#define DISPLAY_RATE_MS    CONFIG_POLYSOM_DISPLAY_RATE_MS
#define SLEEP_DURATION_US  ((uint64_t)CONFIG_POLYSOM_SLEEP_DURATION_MINS * 60ULL * 1000000ULL)

// ── EDF file ──────────────────────────────────────────────────────────────────
// Final path is "/sdcard/biometric_YYYY-MM-DD_HH-MM-SS.edf", timestamped at open time.
#define EDF_FILE_DIR    "/sdcard"
#define EDF_FILE_PREFIX "biometric"

// ── Local time zone ───────────────────────────────────────────────────────────
// UK time: GMT in winter, BST (UTC+1) from last Sunday in March to last Sunday
// in October. SNTP sets the system clock to UTC; this POSIX TZ rule is what
// makes localtime_r() report the correct local wall-clock time.
#define LOCAL_TZ "GMT0BST,M3.5.0/1,M10.5.0"
