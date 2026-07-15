#pragma once
#include "sdkconfig.h"
#include "hal/gpio_types.h"
#include <stdint.h>

// ── I2C ───────────────────────────────────────────────────────────────────────
#define I2C_PORT      I2C_NUM_0
#define I2C_SDA_PIN   GPIO_NUM_2
#define I2C_SCL_PIN   GPIO_NUM_3
#define I2C_FREQ_HZ   400000

// ── MMA8451 accelerometer I2C addresses ──────────────────────────────────────
#define MMA_ADDR_0    0x1C
#define MMA_ADDR_1    0x1D

// ── LDC1612 inductive sensor ──────────────────────────────────────────────────
#define LDC_ADDR      0x2B

// ── SDP800 differential pressure sensor ──────────────────────────────────────
#define SDP800_ADDR   0x25

// ── DS1307 real-time clock ────────────────────────────────────────────────────
#define DS1307_ADDR           0x68
#define RTC_SYNC_INTERVAL_MS  600000

// ── ADC — ECG signal ──────────────────────────────────────────────────────────
// ECG_ADC_UNIT stays as the enum constant; ECG_ADC_CHANNEL is the raw integer
// from Kconfig — callers must cast to adc_channel_t at the call site.
#define ECG_ADC_UNIT    ADC_UNIT_1
#define ECG_ADC_CHANNEL 1

// ── Shared SPI bus (LCD + SD card) ───────────────────────────────────────────
#define SPI_HOST_ID   SPI2_HOST
#define SPI_MOSI_PIN  GPIO_NUM_18
#define SPI_MISO_PIN  GPIO_NUM_20
#define SPI_CLK_PIN   GPIO_NUM_19

// ── ST7789 LCD (172×320) ──────────────────────────────────────────────────────
#define LCD_CS_PIN    GPIO_NUM_14
#define LCD_DC_PIN    GPIO_NUM_15
#define LCD_RST_PIN   GPIO_NUM_21
#define LCD_BL_PIN    GPIO_NUM_22
#define LCD_W         172
#define LCD_H         320
#define LCD_SPI_FREQ  12000000

// ── SD card ───────────────────────────────────────────────────────────────────
#define SD_CS_PIN     GPIO_NUM_21
#define SD_SPI_FREQ   20000000

// ── Sample rates ──────────────────────────────────────────────────────────────
#define ECG_RATE_HZ        100
#define SENSOR_RATE_HZ     50
#define DISPLAY_RATE_HZ    5
#define LOG_RATE_MS        20
#define DISPLAY_RATE_MS    200
#define BLE_RESCAN_MS      10000
#define MAX_SCAN_RETRIES   10
#define SLEEP_DURATION_US  ((uint64_t)10 * 60ULL * 1000000ULL)

// ── EDF file ──────────────────────────────────────────────────────────────────
// Final path is "/sdcard/biometric_YYYY-MM-DD_HH-MM-SS.edf", timestamped at open time.
#define EDF_FILE_DIR    "/sdcard"
#define EDF_FILE_PREFIX "biometric"

// ── Local time zone ───────────────────────────────────────────────────────────
// UK time: GMT in winter, BST (UTC+1) from last Sunday in March to last Sunday
// in October. SNTP sets the system clock to UTC; this POSIX TZ rule is what
// makes localtime_r() report the correct local wall-clock time.
#define LOCAL_TZ "GMT0BST,M3.5.0/1,M10.5.0"
