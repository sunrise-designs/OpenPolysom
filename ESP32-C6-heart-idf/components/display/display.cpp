#include "display.h"
#include "config.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "display";

// ── RGB565 colour palette ─────────────────────────────────────────────────────
#define COL_BG     0x0000u  // black
#define COL_HDR    0xFD20u  // orange
#define COL_LABEL  0x07FFu  // cyan
#define COL_VALUE  0xFFFFu  // white
#define COL_DIV    0x39E7u  // dark grey

#define Y_OFFSET 18

// ── Layout Y positions ────────────────────────────────────────────────────────
#define Y_TITLE     0   + Y_OFFSET
#define Y_DEV       18  + Y_OFFSET
#define Y_HR        30  + Y_OFFSET
#define Y_DIV1      42  + Y_OFFSET
#define Y_A0_LBL    46  + Y_OFFSET
#define Y_A0        57  + Y_OFFSET
#define Y_A1_LBL    79  + Y_OFFSET
#define Y_A1        89  + Y_OFFSET
#define Y_DIV2      111 + Y_OFFSET
#define Y_LDC_LBL   115 + Y_OFFSET
#define Y_CH0       126 + Y_OFFSET
#define Y_CH1       146 + Y_OFFSET
#define Y_WIFI_ST   168 + Y_OFFSET


// ── Backlight (PWM via LEDC) ──────────────────────────────────────────────────
#define BL_LEDC_TIMER    LEDC_TIMER_0
#define BL_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define BL_LEDC_CHANNEL  LEDC_CHANNEL_0
#define BL_LEDC_DUTY_RES LEDC_TIMER_13_BIT
#define BL_LEDC_FREQ_HZ  5000
#define BL_BRIGHTNESS_PCT 5

static void backlight_init(void)
{
    ledc_timer_config_t timer_cfg = {};
    timer_cfg.speed_mode      = BL_LEDC_MODE;
    timer_cfg.timer_num       = BL_LEDC_TIMER;
    timer_cfg.duty_resolution = BL_LEDC_DUTY_RES;
    timer_cfg.freq_hz         = BL_LEDC_FREQ_HZ;
    timer_cfg.clk_cfg         = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t ch_cfg = {};
    ch_cfg.gpio_num   = LCD_BL_PIN;
    ch_cfg.speed_mode = BL_LEDC_MODE;
    ch_cfg.channel    = BL_LEDC_CHANNEL;
    ch_cfg.timer_sel  = BL_LEDC_TIMER;
    ch_cfg.duty       = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

    uint32_t max_duty = (1u << BL_LEDC_DUTY_RES) - 1;
    ESP_ERROR_CHECK(ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, max_duty * BL_BRIGHTNESS_PCT / 100));
    ESP_ERROR_CHECK(ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL));
}

// ── Panel handle ──────────────────────────────────────────────────────────────
static esp_lcd_panel_handle_t s_panel = NULL;

// ── Adafruit 5×7 font — printable ASCII 0x20–0x7E ────────────────────────────
// Source: glcdfont.c (Adafruit GFX library)
// 5 bytes per character, each byte = one column, bit0 = top pixel.
static const uint8_t s_font[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // 0x20 space
    {0x00,0x00,0x5F,0x00,0x00}, // 0x21 !
    {0x00,0x07,0x00,0x07,0x00}, // 0x22 "
    {0x14,0x7F,0x14,0x7F,0x14}, // 0x23 #
    {0x24,0x2A,0x7F,0x2A,0x12}, // 0x24 $
    {0x23,0x13,0x08,0x64,0x62}, // 0x25 %
    {0x36,0x49,0x56,0x20,0x50}, // 0x26 &
    {0x00,0x08,0x07,0x03,0x00}, // 0x27 '
    {0x00,0x1C,0x22,0x41,0x00}, // 0x28 (
    {0x00,0x41,0x22,0x1C,0x00}, // 0x29 )
    {0x2A,0x1C,0x7F,0x1C,0x2A}, // 0x2A *
    {0x08,0x08,0x3E,0x08,0x08}, // 0x2B +
    {0x00,0x80,0x70,0x30,0x00}, // 0x2C ,
    {0x08,0x08,0x08,0x08,0x08}, // 0x2D -
    {0x00,0x00,0x60,0x60,0x00}, // 0x2E .
    {0x20,0x10,0x08,0x04,0x02}, // 0x2F /
    {0x3E,0x51,0x49,0x45,0x3E}, // 0x30 0
    {0x00,0x42,0x7F,0x40,0x00}, // 0x31 1
    {0x72,0x49,0x49,0x49,0x46}, // 0x32 2
    {0x21,0x41,0x49,0x4D,0x33}, // 0x33 3
    {0x18,0x14,0x12,0x7F,0x10}, // 0x34 4
    {0x27,0x45,0x45,0x45,0x39}, // 0x35 5
    {0x3C,0x4A,0x49,0x49,0x31}, // 0x36 6
    {0x41,0x21,0x11,0x09,0x07}, // 0x37 7
    {0x36,0x49,0x49,0x49,0x36}, // 0x38 8
    {0x46,0x49,0x49,0x29,0x1E}, // 0x39 9
    {0x00,0x00,0x14,0x00,0x00}, // 0x3A :
    {0x00,0x40,0x34,0x00,0x00}, // 0x3B ;
    {0x00,0x08,0x14,0x22,0x41}, // 0x3C <
    {0x14,0x14,0x14,0x14,0x14}, // 0x3D =
    {0x00,0x41,0x22,0x14,0x08}, // 0x3E >
    {0x02,0x01,0x59,0x09,0x06}, // 0x3F ?
    {0x3E,0x41,0x5D,0x59,0x4E}, // 0x40 @
    {0x7C,0x12,0x11,0x12,0x7C}, // 0x41 A
    {0x7F,0x49,0x49,0x49,0x36}, // 0x42 B
    {0x3E,0x41,0x41,0x41,0x22}, // 0x43 C
    {0x7F,0x41,0x41,0x41,0x3E}, // 0x44 D
    {0x7F,0x49,0x49,0x49,0x41}, // 0x45 E
    {0x7F,0x09,0x09,0x09,0x01}, // 0x46 F
    {0x3E,0x41,0x41,0x51,0x73}, // 0x47 G
    {0x7F,0x08,0x08,0x08,0x7F}, // 0x48 H
    {0x00,0x41,0x7F,0x41,0x00}, // 0x49 I
    {0x20,0x40,0x41,0x3F,0x01}, // 0x4A J
    {0x7F,0x08,0x14,0x22,0x41}, // 0x4B K
    {0x7F,0x40,0x40,0x40,0x40}, // 0x4C L
    {0x7F,0x02,0x1C,0x02,0x7F}, // 0x4D M
    {0x7F,0x04,0x08,0x10,0x7F}, // 0x4E N
    {0x3E,0x41,0x41,0x41,0x3E}, // 0x4F O
    {0x7F,0x09,0x09,0x09,0x06}, // 0x50 P
    {0x3E,0x41,0x51,0x21,0x5E}, // 0x51 Q
    {0x7F,0x09,0x19,0x29,0x46}, // 0x52 R
    {0x26,0x49,0x49,0x49,0x32}, // 0x53 S
    {0x03,0x01,0x7F,0x01,0x03}, // 0x54 T
    {0x3F,0x40,0x40,0x40,0x3F}, // 0x55 U
    {0x1F,0x20,0x40,0x20,0x1F}, // 0x56 V
    {0x3F,0x40,0x38,0x40,0x3F}, // 0x57 W
    {0x63,0x14,0x08,0x14,0x63}, // 0x58 X
    {0x03,0x04,0x78,0x04,0x03}, // 0x59 Y
    {0x61,0x59,0x49,0x4D,0x43}, // 0x5A Z
    {0x00,0x7F,0x41,0x41,0x41}, // 0x5B [
    {0x02,0x04,0x08,0x10,0x20}, // 0x5C backslash
    {0x00,0x41,0x41,0x41,0x7F}, // 0x5D ]
    {0x04,0x02,0x01,0x02,0x04}, // 0x5E ^
    {0x40,0x40,0x40,0x40,0x40}, // 0x5F _
    {0x00,0x03,0x07,0x08,0x00}, // 0x60 `
    {0x20,0x54,0x54,0x78,0x40}, // 0x61 a
    {0x7F,0x28,0x44,0x44,0x38}, // 0x62 b
    {0x38,0x44,0x44,0x44,0x28}, // 0x63 c
    {0x38,0x44,0x44,0x28,0x7F}, // 0x64 d
    {0x38,0x54,0x54,0x54,0x18}, // 0x65 e
    {0x00,0x08,0x7E,0x09,0x02}, // 0x66 f
    {0x18,0xA4,0xA4,0x9C,0x78}, // 0x67 g
    {0x7F,0x08,0x04,0x04,0x78}, // 0x68 h
    {0x00,0x44,0x7D,0x40,0x00}, // 0x69 i
    {0x20,0x40,0x40,0x3D,0x00}, // 0x6A j
    {0x7F,0x10,0x28,0x44,0x00}, // 0x6B k
    {0x00,0x41,0x7F,0x40,0x00}, // 0x6C l
    {0x7C,0x04,0x78,0x04,0x78}, // 0x6D m
    {0x7C,0x08,0x04,0x04,0x78}, // 0x6E n
    {0x38,0x44,0x44,0x44,0x38}, // 0x6F o
    {0xFC,0x18,0x24,0x24,0x18}, // 0x70 p
    {0x18,0x24,0x24,0x18,0xFC}, // 0x71 q
    {0x7C,0x08,0x04,0x04,0x08}, // 0x72 r
    {0x48,0x54,0x54,0x54,0x24}, // 0x73 s
    {0x04,0x04,0x3F,0x44,0x24}, // 0x74 t
    {0x3C,0x40,0x40,0x20,0x7C}, // 0x75 u
    {0x1C,0x20,0x40,0x20,0x1C}, // 0x76 v
    {0x3C,0x40,0x30,0x40,0x3C}, // 0x77 w
    {0x44,0x28,0x10,0x28,0x44}, // 0x78 x
    {0x4C,0x90,0x90,0x90,0x7C}, // 0x79 y
    {0x44,0x64,0x54,0x4C,0x44}, // 0x7A z
    {0x00,0x08,0x36,0x41,0x00}, // 0x7B {
    {0x00,0x00,0x77,0x00,0x00}, // 0x7C |
    {0x00,0x41,0x36,0x08,0x00}, // 0x7D }
    {0x02,0x01,0x02,0x04,0x02}, // 0x7E ~
};

// ── Line buffer (one row, full LCD width) ─────────────────────────────────────
// Must be 32-bit aligned for DMA.
static uint16_t WORD_ALIGNED_ATTR s_line[LCD_W];

// ── Low-level draw helpers ────────────────────────────────────────────────────
// ST7789 expects RGB565 big-endian; ESP32 is little-endian → swap bytes.
static inline uint16_t swap16(uint16_t c) { return (c >> 8) | (c << 8); }

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    uint16_t c = swap16(color);
    for (int i = 0; i < w; i++) s_line[i] = c;
    for (int row = y; row < y + h; row++)
        esp_lcd_panel_draw_bitmap(s_panel, x, row, x + w, row + 1, s_line);
}

// Render one character cell (6×8 pixels). Char cell = 5 pixel columns + 1 gap column.
static uint16_t WORD_ALIGNED_ATTR s_char_buf[6 * 8];

static void draw_char(int x, int y, char c, uint16_t fg, uint16_t bg)
{
    if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) c = '?';
    const uint8_t *glyph = s_font[(uint8_t)c - 0x20];
    uint16_t fg_s = swap16(fg), bg_s = swap16(bg);

    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 6; col++) {
            uint8_t bit = (col < 5) ? ((glyph[col] >> row) & 1) : 0;
            s_char_buf[row * 6 + col] = bit ? fg_s : bg_s;
        }
    }
    esp_lcd_panel_draw_bitmap(s_panel, x, y, x + 6, y + 8, s_char_buf);
}

static void draw_string(int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    for (; *s; s++, x += 6)
        draw_char(x, y, *s, fg, bg);
}

static void draw_hline(int x, int y, int w, uint16_t color)
{
    fill_rect(x, y, w, 1, color);
}

// ── Static labels (drawn once at init) ────────────────────────────────────────
static void draw_labels(void)
{
    fill_rect(0, 0, LCD_W, LCD_H, COL_BG);

    draw_string(0, Y_TITLE,   "Polar H9",        COL_HDR,   COL_BG);
    // 0x180D is the Bluetooth SIG Heart Rate Service UUID — a fixed constant.
    draw_string(100, Y_TITLE + 4, "SVC:0x180D", COL_LABEL, COL_BG);
    draw_hline(0, Y_DIV1,  LCD_W, COL_DIV);
    draw_string(0, Y_A0_LBL, "-- Accel 0 --",    COL_LABEL, COL_BG);
    draw_string(0, Y_A1_LBL, "-- Accel 1 --",    COL_LABEL, COL_BG);
    draw_hline(0, Y_DIV2,  LCD_W, COL_DIV);
    draw_string(0, Y_LDC_LBL,"-- LDC1612 --",    COL_LABEL, COL_BG);
}

// ── Physics helpers ───────────────────────────────────────────────────────────
static void accel_angles(int16_t ax, int16_t ay, int16_t az,
                         float *mag_mg, float *pitch, float *roll)
{
    float x = ax, y = ay, z = az;
    *mag_mg = sqrtf(x*x + y*y + z*z) * (2000.0f / 8192.0f);
    *pitch  = atan2f(-x, sqrtf(y*y + z*z)) * (180.0f / (float)M_PI);
    *roll   = atan2f(y, z)                  * (180.0f / (float)M_PI);
}

// ── Public API ────────────────────────────────────────────────────────────────
void display_init(void)
{
    // SPI bus (shared with SD card — logger_init() adds SD as a second device)
    spi_bus_config_t bus = {};
    bus.mosi_io_num     = SPI_MOSI_PIN;
    bus.miso_io_num     = SPI_MISO_PIN;
    bus.sclk_io_num     = SPI_CLK_PIN;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = LCD_W * 40 * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST_ID, &bus, SPI_DMA_CH_AUTO));

    // Panel IO (SPI device for LCD)
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num       = LCD_CS_PIN;
    io_cfg.dc_gpio_num       = LCD_DC_PIN;
    io_cfg.pclk_hz           = LCD_SPI_FREQ;
    io_cfg.lcd_cmd_bits      = 8;
    io_cfg.lcd_param_bits    = 8;
    io_cfg.trans_queue_depth = 10;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)SPI_HOST_ID, &io_cfg, &io));

    // ST7789 panel
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = LCD_RST_PIN;
    panel_cfg.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR;  // BGR — matches reference board
    panel_cfg.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel));

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_set_gap(s_panel, 34, 0);  // GRAM X offset — 172px active area starts at col 34
    esp_lcd_panel_mirror(s_panel, true, true);  // mirror X — matches reference board
    esp_lcd_panel_disp_on_off(s_panel, true);

    backlight_init();

    draw_labels();
    ESP_LOGI(TAG, "Display ready");
}

void display_update(const display_data_t *data)
{
    char buf[32];

    // ── Device / connection status ────────────────────────────────────────────
    fill_rect(0, Y_DEV, LCD_W, 10, COL_BG);
    draw_string(0, Y_DEV,
                data->ble_connected ? data->ble_device_name : "Scanning...",
                COL_VALUE, COL_BG);

    // ── Heart rate / RR interval ──────────────────────────────────────────────
    fill_rect(0, Y_HR, LCD_W, 10, COL_BG);
    snprintf(buf, sizeof(buf), "HR:%3ubpm  RR:%4ums",
             (unsigned)data->bpm, (unsigned)data->rr_ms);
    draw_string(0, Y_HR, buf, COL_VALUE, COL_BG);

    // ── Accelerometer 0 ───────────────────────────────────────────────────────
    float mag0, p0, r0;
    accel_angles(data->accel0[0], data->accel0[1], data->accel0[2], &mag0, &p0, &r0);
    fill_rect(0, Y_A0, LCD_W, 20, COL_BG);
    snprintf(buf, sizeof(buf), "  Mag:%5.0f mg", mag0);
    draw_string(0, Y_A0, buf, COL_VALUE, COL_BG);
    snprintf(buf, sizeof(buf), "  P:%+6.1f  R:%+6.1f", p0, r0);
    draw_string(0, Y_A0 + 10, buf, COL_VALUE, COL_BG);

    // ── Accelerometer 1 ───────────────────────────────────────────────────────
    float mag1, p1, r1;
    accel_angles(data->accel1[0], data->accel1[1], data->accel1[2], &mag1, &p1, &r1);
    fill_rect(0, Y_A1, LCD_W, 20, COL_BG);
    snprintf(buf, sizeof(buf), "  Mag:%5.0f mg", mag1);
    draw_string(0, Y_A1, buf, COL_VALUE, COL_BG);
    snprintf(buf, sizeof(buf), "  P:%+6.1f  R:%+6.1f", p1, r1);
    draw_string(0, Y_A1 + 10, buf, COL_VALUE, COL_BG);

    // ── LDC1612 ───────────────────────────────────────────────────────────────
    fill_rect(0, Y_CH0, LCD_W, 20, COL_BG);
    snprintf(buf, sizeof(buf), "  CH0:%9lu", (unsigned long)data->ldc0);
    draw_string(0, Y_CH0, buf, COL_VALUE, COL_BG);
    if (data->baseline_ok)
        snprintf(buf, sizeof(buf), "  d0:%+10ld", (long)(int32_t)(data->ldc0 - data->ldc0_baseline));
    else
        snprintf(buf, sizeof(buf), "  d0: (no base)");
    draw_string(0, Y_CH0 + 10, buf, COL_VALUE, COL_BG);

    fill_rect(0, Y_CH1, LCD_W, 20, COL_BG);
    snprintf(buf, sizeof(buf), "  CH1:%9lu", (unsigned long)data->ldc1);
    draw_string(0, Y_CH1, buf, COL_VALUE, COL_BG);
    if (data->baseline_ok)
        snprintf(buf, sizeof(buf), "  d1:%+10ld", (long)(int32_t)(data->ldc1 - data->ldc1_baseline));
    else
        snprintf(buf, sizeof(buf), "  d1: (no base)");
    draw_string(0, Y_CH1 + 10, buf, COL_VALUE, COL_BG);
}
