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

// ── ADC — ECG signal ──────────────────────────────────────────────────────────
// ECG_ADC_UNIT stays as the enum constant; ECG_ADC_CHANNEL is the raw integer
// from Kconfig — callers must cast to adc_channel_t at the call site.
#define ECG_ADC_UNIT    ADC_UNIT_1
#define ECG_ADC_CHANNEL CONFIG_POLYSOM_ECG_ADC_CHANNEL

// ── Shared SPI bus (LCD + SD card) ───────────────────────────────────────────
#define SPI_HOST_ID   SPI2_HOST
#define SPI_MOSI_PIN  ((gpio_num_t)CONFIG_POLYSOM_SPI_MOSI_PIN)
#define SPI_MISO_PIN  ((gpio_num_t)CONFIG_POLYSOM_SPI_MISO_PIN)
#define SPI_CLK_PIN   ((gpio_num_t)CONFIG_POLYSOM_SPI_CLK_PIN)

// ── ST7789 LCD (172×320) ──────────────────────────────────────────────────────
#define LCD_CS_PIN    ((gpio_num_t)CONFIG_POLYSOM_LCD_CS_PIN)
#define LCD_DC_PIN    ((gpio_num_t)CONFIG_POLYSOM_LCD_DC_PIN)
#define LCD_RST_PIN   ((gpio_num_t)CONFIG_POLYSOM_LCD_RST_PIN)
#define LCD_BL_PIN    ((gpio_num_t)CONFIG_POLYSOM_LCD_BL_PIN)
#define LCD_W         CONFIG_POLYSOM_LCD_W
#define LCD_H         CONFIG_POLYSOM_LCD_H
#define LCD_SPI_FREQ  CONFIG_POLYSOM_LCD_SPI_FREQ_HZ

// ── SD card ───────────────────────────────────────────────────────────────────
#define SD_CS_PIN     ((gpio_num_t)CONFIG_POLYSOM_SD_CS_PIN)
#define SD_SPI_FREQ   CONFIG_POLYSOM_SD_SPI_FREQ_HZ

// ── Sample rates ──────────────────────────────────────────────────────────────
#define ECG_RATE_HZ        CONFIG_POLYSOM_ECG_RATE_HZ
#define SENSOR_RATE_HZ     CONFIG_POLYSOM_SENSOR_RATE_HZ
#define DISPLAY_RATE_HZ    CONFIG_POLYSOM_DISPLAY_RATE_HZ
#define LOG_RATE_MS        CONFIG_POLYSOM_LOG_RATE_MS
#define DISPLAY_RATE_MS    CONFIG_POLYSOM_DISPLAY_RATE_MS
#define BLE_RESCAN_MS      CONFIG_POLYSOM_BLE_RESCAN_MS
#define MAX_SCAN_RETRIES   CONFIG_POLYSOM_MAX_SCAN_RETRIES
#define SLEEP_DURATION_US  ((uint64_t)CONFIG_POLYSOM_SLEEP_DURATION_MINS * 60ULL * 1000000ULL)

// ── EDF file ──────────────────────────────────────────────────────────────────
#define EDF_FILE_PATH "/sdcard/biometric.edf"
