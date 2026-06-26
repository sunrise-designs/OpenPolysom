#pragma once
#include <stdint.h>

// ── ST7789 LCD pins (172×320, software SPI) ── PLACEHOLDER ───────────────────
// Set these to match the board schematic (Waveshare ESP32-C6-LCD-1.47).
// Software SPI is used so the LCD does not share the SD card's FSPI peripheral.
#define LCD_CS_PIN    4   // chip select
#define LCD_DC_PIN    2   // data/command
#define LCD_RST_PIN   3   // reset
#define LCD_MOSI_PIN  9   // SPI data out (MISO not needed — LCD is write-only)
#define LCD_CLK_PIN   8   // SPI clock
#define LCD_BL_PIN    5   // backlight (active HIGH)

#define LCD_W  172
#define LCD_H  320

#define DISPLAY_RATE_MS  200   // 5 Hz refresh

void initDisplay();
void updateDisplay();
