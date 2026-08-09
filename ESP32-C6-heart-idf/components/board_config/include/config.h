#pragma once
#include "sdkconfig.h"
#include "hal/gpio_types.h"
#include <stdint.h>

// ── I2C ───────────────────────────────────────────────────────────────────────
#define I2C_PORT      I2C_NUM_0
#define I2C_SDA_PIN   GPIO_NUM_22
#define I2C_SCL_PIN   GPIO_NUM_23
#define I2C_FREQ_HZ   100000

// ── MMA8451 accelerometer I2C addresses ──────────────────────────────────────
#define MMA_ADDR_0    0x1C
#define MMA_ADDR_1    0x1D

// ── LDC1612 inductive sensor ──────────────────────────────────────────────────
#define LDC_ADDR      0x2B

// ── SDP800 differential pressure sensor ──────────────────────────────────────
#define SDP800_ADDR   0x25

// ── DS3231 real-time clock ────────────────────────────────────────────────────
#define DS3231_ADDR           0x68
#define RTC_SYNC_INTERVAL_MS  600000

// ── Serial time sync (used when no RTC is fitted / holds no valid time) ─────
#define TIME_SYNC_TIMEOUT_MS  30000

// ── ADC — ECG signal (AD8232, raw, logged for later analysis) ────────────────
// ECG_ADC_UNIT stays as the enum constant; ECG_ADC_CHANNEL is the raw integer
// from Kconfig — callers must cast to adc_channel_t at the call site.
#define ECG_ADC_UNIT    ADC_UNIT_1
#define ECG_ADC_CHANNEL 1

// ── SH1106 OLED (128×64, I2C — shares the sensor I2C bus above) ──────────────
#define OLED_ADDR     0x3C
#define OLED_W        128
#define OLED_H        64

// ── SPI bus (SD card only — the LCD used to share this bus, now I2C above) ──
#define SPI_HOST_ID   SPI2_HOST
#define SPI_MOSI_PIN  GPIO_NUM_18
#define SPI_MISO_PIN  GPIO_NUM_20
#define SPI_CLK_PIN   GPIO_NUM_19

// ── SD card ───────────────────────────────────────────────────────────────────
#define SD_CS_PIN     GPIO_NUM_21
#define SD_SPI_FREQ   20000000

// ── User LED ──────────────────────────────────────────────────────────────────
// Active-high indicator; blinked to confirm a successful serial time sync.
#define USER_LED_PIN  GPIO_NUM_15

// ── Sample rates ──────────────────────────────────────────────────────────────
#define ECG_RATE_HZ        100
#define SENSOR_RATE_HZ     50
#define DISPLAY_RATE_HZ    5
#define LOG_RATE_MS        20
#define DISPLAY_RATE_MS    200

// ── EDF file ──────────────────────────────────────────────────────────────────
// Final path is "/sdcard/biometric_YYYY-MM-DD_HH-MM-SS.edf", timestamped at open time.
#define EDF_FILE_DIR    "/sdcard"
#define EDF_FILE_PREFIX "biometric"

// ── Real-time streaming (components/rt_stream) ───────────────────────────────
// Live sample streaming over Wi-Fi, for watching the signals while a recording
// runs. OFF by default: the radio costs the power this firmware otherwise saves
// (see ../wiki/knowledge/hardware.md), so a normal unattended night is
// unaffected. Enable per-device over the serial command channel, which persists
// the choice in NVS; RT_STREAM_ENABLED_DEFAULT is only the value used before
// anything has ever been stored.
#define RT_STREAM_ENABLED_DEFAULT   false

// SoftAP by default — a bedside device with no network to join, and live
// physiological data that has no business on a home LAN. The SSID gets the last
// three MAC bytes appended so two units in one room are distinguishable.
#define RT_STREAM_SOFTAP            1
#define RT_STREAM_SSID_PREFIX       "ProtoSom-"
#define RT_STREAM_AP_CHANNEL        6
#define RT_STREAM_AP_MAX_CONN       2

// WPA2 password, derived per-device from the MAC and shown on the OLED, so it
// is neither blank nor the same on every unit. See rt_stream_password().
#define RT_STREAM_PASSWORD_PREFIX   "som"

// STA mode (join an existing network) — only used when RT_STREAM_SOFTAP is 0.
#define RT_STREAM_STA_SSID          ""
#define RT_STREAM_STA_PASSWORD      ""

#define RT_STREAM_PORT              80
#define RT_STREAM_WS_PATH           "/rt"

// One frame per 100 ms: 5 samples of each 50 Hz channel, 10 of ECG. Matches the
// cadence src_web/src/rt_store.ts is tuned for, and keeps a frame comfortably
// inside one TCP segment at ~4 KB of JSON.
#define RT_STREAM_FRAME_MS          100

// Queue depth in frames. 2 s of buffering: enough to ride out a retransmit,
// short enough that a wedged client is dropped rather than accumulating stale
// samples nobody will look at.
#define RT_STREAM_QUEUE_FRAMES      20

// Stop the radio after this long with no client connected.
#define RT_STREAM_IDLE_TIMEOUT_S    600

// ── Local time zone ───────────────────────────────────────────────────────────
// UK time: GMT in winter, BST (UTC+1) from last Sunday in March to last Sunday
// in October. SNTP sets the system clock to UTC; this POSIX TZ rule is what
// makes localtime_r() report the correct local wall-clock time.
#define LOCAL_TZ "GMT0BST,M3.5.0/1,M10.5.0"
