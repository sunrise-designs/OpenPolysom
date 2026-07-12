#include "display.h"
#include "config.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "display";

// ── Layout — 128x64, 8 rows of 8px (one SH1106 GDDRAM page each) ─────────────
#define X_OFFSET 0
#define Y_TITLE  0
#define Y_A0     8
#define Y_A1     16
#define Y_LDC    24
#define Y_FLOW   32
#define Y_REC    40
#define Y_TIME   48
#define Y_ROW7   56  // boot message if set, else Wi-Fi SSID

// ── I2C handles ───────────────────────────────────────────────────────────────
static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static bool s_sleeping = false;

// Boot message, redrawn every display_update() cycle (row 7) so it survives
// the full-framebuffer clear each frame does. Persists until the next reboot.
static char s_boot_msg[24] = "";

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

// ── Monochrome framebuffer ────────────────────────────────────────────────────
// One byte per column per 8-row page, matching SH1106 GDDRAM layout exactly —
// a page's row is a single contiguous I2C data burst (see oled_flush()).
static uint8_t s_fb[OLED_H / 8][OLED_W];

static inline void set_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) return;
    uint8_t bit = 1u << (y & 7);
    if (on) s_fb[y >> 3][x] |= bit;
    else    s_fb[y >> 3][x] &= (uint8_t)~bit;
}

static void fill_rect(int x, int y, int w, int h, bool on)
{
    if (x + w > OLED_W) w = OLED_W - x;
    if (y + h > OLED_H) h = OLED_H - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            set_pixel(xx, yy, on);
}

static void draw_hline(int x, int y, int w, bool on)
{
    fill_rect(x, y, w, 1, on);
}

static void clear_fb(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

// Render one character cell (6×8 pixels: 5 glyph columns + 1 gap column).
// Only "on" bits are set — the caller is expected to have cleared the
// framebuffer already (display_update() does this once per frame), so there
// is no separate background colour to paint.
static void draw_char(int x, int y, char c)
{
    if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) c = '?';
    const uint8_t *glyph = s_font[(uint8_t)c - 0x20];
    for (int col = 0; col < 5; col++)
        for (int row = 0; row < 8; row++)
            if ((glyph[col] >> row) & 1)
                set_pixel(x + col, y + row, true);
}

static void draw_string(int x, int y, const char *s)
{
    for (; *s; s++, x += 6)
        draw_char(x, y, *s);
}

// ── SH1106 I2C transport ──────────────────────────────────────────────────────
// Protocol: first byte is a control byte (0x00 = command stream, 0x40 = data
// stream); every following byte in the same I2C transaction is interpreted
// sequentially by the controller, so a whole init sequence or a whole
// framebuffer page can be sent as one burst.
static void oled_write_cmds(const uint8_t *cmds, size_t n)
{
    uint8_t buf[32];
    buf[0] = 0x00;
    memcpy(buf + 1, cmds, n);
    i2c_master_transmit(s_dev, buf, n + 1, 100);
}

static void oled_write_cmd(uint8_t cmd)
{
    oled_write_cmds(&cmd, 1);
}

static void oled_write_page(uint8_t page, const uint8_t *data)
{
    // SH1106 drives a 132-segment column RAM behind a 128-pixel panel — the
    // panel's column 0 sits at RAM column 2, hence the +2 column offset.
    uint8_t page_cmds[3] = { (uint8_t)(0xB0 | page), 0x02, 0x10 };
    oled_write_cmds(page_cmds, 3);

    uint8_t buf[1 + OLED_W];
    buf[0] = 0x40;
    memcpy(buf + 1, data, OLED_W);
    i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

static void oled_flush(void)
{
    for (int page = 0; page < OLED_H / 8; page++)
        oled_write_page((uint8_t)page, s_fb[page]);
}

// Title + divider only — shown at boot/wake before the first real
// display_update() (from the sensor task, at 5 Hz) fills in live values.
static void draw_static_frame(void)
{
    clear_fb();
    draw_string(X_OFFSET, Y_TITLE, "PolySom");
    draw_hline(0, Y_TITLE + 7, OLED_W, true);
    oled_flush();
}

void display_boot_msg(const char *msg)
{
    if (s_sleeping) return;
    strncpy(s_boot_msg, msg, sizeof(s_boot_msg) - 1);
    s_boot_msg[sizeof(s_boot_msg) - 1] = '\0';
    fill_rect(0, Y_ROW7, OLED_W, 8, false);
    draw_string(X_OFFSET, Y_ROW7, s_boot_msg);
    oled_flush();
}

void display_sleep(void)
{
    if (s_sleeping) return;
    oled_write_cmd(0xAE);  // display off
    s_sleeping = true;
    ESP_LOGI(TAG, "Display asleep");
}

void display_wake(void)
{
    if (!s_sleeping) return;
    oled_write_cmd(0xAF);  // display on
    s_sleeping = false;
    draw_static_frame();
    ESP_LOGI(TAG, "Display awake");
}

bool display_is_sleeping(void)
{
    return s_sleeping;
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
i2c_master_bus_handle_t display_get_i2c_bus(void)
{
    return s_bus;
}

void display_init(void)
{
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port          = I2C_PORT;
    bus_cfg.sda_io_num        = I2C_SDA_PIN;
    bus_cfg.scl_io_num        = I2C_SCL_PIN;
    bus_cfg.clk_source        = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_bus));

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = OLED_ADDR;
    dev_cfg.scl_speed_hz    = I2C_FREQ_HZ;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev));

    static const uint8_t init_cmds[] = {
        0xAE,             // display off
        0xD5, 0x80,       // clock divide ratio / oscillator frequency
        0xA8, OLED_H - 1, // multiplex ratio
        0xD3, 0x00,       // display offset
        0x40,             // display start line 0
        0xAD, 0x8B,       // charge pump (DC-DC) enable — SH1106-specific
        0xA1,             // segment remap
        0xC8,             // COM output scan direction, remapped
        0xDA, 0x12,       // COM pins hardware config
        0x81, 0x80,       // contrast
        0xD9, 0xF1,       // pre-charge period
        0xDB, 0x40,       // VCOM deselect level
        0xA4,             // resume to RAM content display
        0xA6,             // normal display (not inverted)
        0xAF,             // display on
    };
    oled_write_cmds(init_cmds, sizeof(init_cmds));

    draw_static_frame();
    ESP_LOGI(TAG, "Display ready");
}

void display_update(const display_data_t *data)
{
    if (s_sleeping) return;

    clear_fb();
    char buf[24];  // 128px / 6px per char = 21 chars max per row

    // ── Title + battery ───────────────────────────────────────────────────────
    snprintf(buf, sizeof(buf), "PolySom  B:%3u%%", (unsigned)data->batt_percent);
    draw_string(X_OFFSET, Y_TITLE, buf);
    draw_hline(0, Y_TITLE + 7, OLED_W, true);

    // ── Accelerometer 0 ───────────────────────────────────────────────────────
    float mag0, p0, r0;
    accel_angles(data->accel0[0], data->accel0[1], data->accel0[2], &mag0, &p0, &r0);
    snprintf(buf, sizeof(buf), "A0 M%4.0f P%+4.0f R%+4.0f", mag0, p0, r0);
    draw_string(X_OFFSET, Y_A0, buf);

    // ── Accelerometer 1 ───────────────────────────────────────────────────────
    float mag1, p1, r1;
    accel_angles(data->accel1[0], data->accel1[1], data->accel1[2], &mag1, &p1, &r1);
    snprintf(buf, sizeof(buf), "A1 M%4.0f P%+4.0f R%+4.0f", mag1, p1, r1);
    draw_string(X_OFFSET, Y_A1, buf);

    // ── LDC1612 (raw counts — no baseline on this screen) ────────────────────
    snprintf(buf, sizeof(buf), "CH0:%6lu CH1:%6lu",
             (unsigned long)data->ldc0, (unsigned long)data->ldc1);
    draw_string(X_OFFSET, Y_LDC, buf);

    // ── Air flow ──────────────────────────────────────────────────────────────
    snprintf(buf, sizeof(buf), "Flow:%8.1f mbar", data->pressure_mbar);
    draw_string(X_OFFSET, Y_FLOW, buf);

    // ── Recording status ──────────────────────────────────────────────────────
    if (!data->recording) {
        draw_string(X_OFFSET, Y_REC, "NOT RECORDING - NO SD");
    } else {
        snprintf(buf, sizeof(buf), "RECORDING  %lus", (unsigned long)data->recording_seconds);
        draw_string(X_OFFSET, Y_REC, buf);
    }

    // ── Current time ──────────────────────────────────────────────────────────
    // Reflects whichever source set the system clock at boot (DS1307 RTC or
    // a serial time-sync command — see main.cpp/sensors.cpp/time_sync.c); a
    // year sanity check catches the case where neither sync succeeded and the
    // clock is still at its epoch default.
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year + 1900 >= 2020) {
        // Casts + modulo bound each field's width for the compiler — plain
        // int tm_* fields make -Wformat-truncation assume up to 11 digits.
        snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u",
                 (unsigned)(t.tm_year + 1900) % 10000u,
                 (unsigned)(t.tm_mon + 1) % 100u,
                 (unsigned)t.tm_mday % 100u,
                 (unsigned)t.tm_hour % 100u,
                 (unsigned)t.tm_min % 100u,
                 (unsigned)t.tm_sec % 100u);
        draw_string(X_OFFSET, Y_TIME, buf);
    } else {
        draw_string(X_OFFSET, Y_TIME, "No time sync");
    }

    // ── Row 7: boot message (sticky) if set, else time-sync source ───────────
    if (s_boot_msg[0] != '\0') {
        draw_string(X_OFFSET, Y_ROW7, s_boot_msg);
    } else {
        const char *src = (data->time_sync_source && data->time_sync_source[0])
                              ? data->time_sync_source
                              : "none";
        snprintf(buf, sizeof(buf), "Time sync: %s", src);
        draw_string(X_OFFSET, Y_ROW7, buf);
    }

    oled_flush();
}
