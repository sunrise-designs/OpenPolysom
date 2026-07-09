#include "display.h"
#include "ble.h"
#include "logger.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <math.h>
#include <Arduino.h>

// Sensor globals defined in the .ino
extern volatile int16_t  latestAccel0X, latestAccel0Y, latestAccel0Z;
extern volatile int16_t  latestAccel1X, latestAccel1Y, latestAccel1Z;
extern volatile uint32_t latestLdc0, latestLdc1;

// ── RGB565 palette ────────────────────────────────────────────────────────────
#define COL_BG     0x0000u   // black
#define COL_HDR    0xFD20u   // orange  — "Polar H9" header
#define COL_LABEL  0x07FFu   // cyan    — section dividers / labels
#define COL_VALUE  0xFFFFu   // white   — live values
#define COL_DIV    0x39E7u   // dark grey — horizontal rule

// ── Fixed Y positions for value fields (erased + redrawn each update) ────────
//
//   y=  0  "Polar H9"            textSize 2, static
//   y= 18  [device name]         textSize 1, value
//   y= 30  [HR bpm / RR ms]      textSize 1, value
//             divider @ y=42
//   y= 46  "-- Accel 0 --"       textSize 1, static label
//   y= 57  [mag]                 textSize 1, value
//   y= 67  [pitch / roll]        textSize 1, value
//   y= 79  "-- Accel 1 --"       textSize 1, static label
//   y= 89  [mag]                 textSize 1, value
//   y= 99  [pitch / roll]        textSize 1, value
//            divider @ y=111
//   y=115  "-- LDC1612 --"       textSize 1, static label
//   y=126  [CH0 raw]             textSize 1, value
//   y=136  [CH0 delta]           textSize 1, value
//   y=146  [CH1 raw]             textSize 1, value
//   y=156  [CH1 delta]           textSize 1, value

static const int16_t Y_DEV  = 18;
static const int16_t Y_HR   = 30;
static const int16_t Y_A0   = 57;   // two rows: Y_A0 and Y_A0+10
static const int16_t Y_A1   = 89;   // two rows: Y_A1 and Y_A1+10
static const int16_t Y_CH0  = 126;  // two rows: Y_CH0 and Y_CH0+10
static const int16_t Y_CH1  = 146;  // two rows: Y_CH1 and Y_CH1+10

static Adafruit_ST7789 tft(LCD_CS_PIN, LCD_DC_PIN, LCD_MOSI_PIN, LCD_CLK_PIN, LCD_RST_PIN);

static void eraseRow(int16_t y, int16_t h = 10) {
    tft.fillRect(0, y, LCD_W, h, COL_BG);
}

static void drawLabels() {
    tft.setTextWrap(false);

    tft.setTextSize(2);
    tft.setTextColor(COL_HDR);
    tft.setCursor(0, 0);
    tft.print("Polar H9");

    tft.setTextSize(1);
    tft.setTextColor(COL_LABEL);
    tft.drawFastHLine(0, 42, LCD_W, COL_DIV);
    tft.setCursor(0, 46);  tft.print("-- Accel 0 --");
    tft.setCursor(0, 79);  tft.print("-- Accel 1 --");
    tft.drawFastHLine(0, 111, LCD_W, COL_DIV);
    tft.setCursor(0, 115); tft.print("-- LDC1612 --");
}

// Converts raw 14-bit MMA8451 counts (±8192 = ±2000 mg at 2 G range)
// into magnitude (mg), pitch and roll (degrees).
static void accelAngles(int16_t ax, int16_t ay, int16_t az,
                        float &mag_mg, float &pitch, float &roll) {
    float x = (float)ax, y = (float)ay, z = (float)az;
    mag_mg = sqrtf(x*x + y*y + z*z) * (2000.0f / 8192.0f);
    pitch  = atan2f(-x, sqrtf(y*y + z*z)) * RAD_TO_DEG;
    roll   = atan2f(y, z) * RAD_TO_DEG;
}

void initDisplay() {
    pinMode(LCD_BL_PIN, OUTPUT);
    digitalWrite(LCD_BL_PIN, HIGH);

    tft.init(LCD_W, LCD_H);
    tft.setRotation(0);
    tft.fillScreen(COL_BG);
    drawLabels();
}

void updateDisplay() {
    char buf[32];
    tft.setTextWrap(false);
    tft.setTextSize(1);
    tft.setTextColor(COL_VALUE);

    // ── Device name / BLE address ─────────────────────────────────────────────
    eraseRow(Y_DEV);
    tft.setCursor(0, Y_DEV);
    if (connected && bleDeviceName.length() > 0)
        tft.print(bleDeviceName.c_str());
    else if (connected)
        tft.print(bleDeviceAddress.c_str());
    else
        tft.print("Scanning...");

    // ── Heart rate / RR interval ──────────────────────────────────────────────
    eraseRow(Y_HR);
    tft.setCursor(0, Y_HR);
    snprintf(buf, sizeof(buf), "HR:%3ubpm  RR:%4ums",
             (unsigned)latestBpm, (unsigned)latestRR_ms);
    tft.print(buf);

    // ── Accelerometer 0 ───────────────────────────────────────────────────────
    float mag0, p0, r0;
    accelAngles(latestAccel0X, latestAccel0Y, latestAccel0Z, mag0, p0, r0);
    eraseRow(Y_A0, 20);
    tft.setCursor(0, Y_A0);
    snprintf(buf, sizeof(buf), "  Mag:%5.0f mg", mag0);
    tft.print(buf);
    tft.setCursor(0, Y_A0 + 10);
    snprintf(buf, sizeof(buf), "  P:%+6.1f  R:%+6.1f", p0, r0);
    tft.print(buf);

    // ── Accelerometer 1 ───────────────────────────────────────────────────────
    float mag1, p1, r1;
    accelAngles(latestAccel1X, latestAccel1Y, latestAccel1Z, mag1, p1, r1);
    eraseRow(Y_A1, 20);
    tft.setCursor(0, Y_A1);
    snprintf(buf, sizeof(buf), "  Mag:%5.0f mg", mag1);
    tft.print(buf);
    tft.setCursor(0, Y_A1 + 10);
    snprintf(buf, sizeof(buf), "  P:%+6.1f  R:%+6.1f", p1, r1);
    tft.print(buf);

    // ── LDC1612 ───────────────────────────────────────────────────────────────
    uint32_t base0 = getLdc0Baseline();
    uint32_t base1 = getLdc1Baseline();
    bool     bOk   = getLdcBaselineOk();

    eraseRow(Y_CH0, 20);
    tft.setCursor(0, Y_CH0);
    snprintf(buf, sizeof(buf), "  CH0:%9lu", (unsigned long)latestLdc0);
    tft.print(buf);
    tft.setCursor(0, Y_CH0 + 10);
    if (bOk)
        snprintf(buf, sizeof(buf), "  d0: %+10ld", (long)(int32_t)(latestLdc0 - base0));
    else
        snprintf(buf, sizeof(buf), "  d0: (no base)");
    tft.print(buf);

    eraseRow(Y_CH1, 20);
    tft.setCursor(0, Y_CH1);
    snprintf(buf, sizeof(buf), "  CH1:%9lu", (unsigned long)latestLdc1);
    tft.print(buf);
    tft.setCursor(0, Y_CH1 + 10);
    if (bOk)
        snprintf(buf, sizeof(buf), "  d1: %+10ld", (long)(int32_t)(latestLdc1 - base1));
    else
        snprintf(buf, sizeof(buf), "  d1: (no base)");
    tft.print(buf);
}
