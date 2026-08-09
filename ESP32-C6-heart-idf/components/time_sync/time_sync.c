#include "time_sync.h"
#include "config.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "time_sync";

#pragma pack(push, 1)
typedef struct {
    uint8_t magic[2];
    int64_t unix_time_s;
    char    tz[TIME_SYNC_TZ_LEN];
    uint8_t checksum;
} time_sync_frame_t;
#pragma pack(pop)

#define FRAME_MAGIC0 0xA5
#define FRAME_MAGIC1 0x5A
#define FRAME_SIZE   sizeof(time_sync_frame_t)

// Second command on the same link: enable/disable real-time streaming. Given
// its own magic-1 byte rather than a type field inside the time frame, so the
// existing tools/set_time.py wire format is untouched. See time_sync.h.
#define RT_CMD_MAGIC1 0x5B
#define RT_CMD_BODY   2   // enabled byte + checksum

// Once the magic bytes are seen the rest of the 51-byte frame is milliseconds
// behind them at 115200 baud, so a body that doesn't arrive inside this window
// is a truncated/garbled frame: abandon it and go back to hunting magic rather
// than blocking forever half-way through a parse.
#define FRAME_BODY_TIMEOUT_MS 1000

// ── User LED (see USER_LED_PIN in board_config) ──────────────────────────────
// Flashed on every accepted frame. tools/set_time.py gives no on-device
// confirmation of its own, and a mid-recording sync happens with the display
// already asleep, so the LED is the only feedback the host operator gets.
#define LED_BLINK_COUNT     5
#define LED_BLINK_HALF_MS   250   // 250 ms on + 250 ms off = 2 Hz

static SemaphoreHandle_t  s_synced    = NULL;  // given once per accepted frame
static time_sync_cb_t     s_on_sync   = NULL;
static time_sync_rt_cb_t  s_on_rt_cmd = NULL;

static void user_led_blink(void)
{
    for (int i = 0; i < LED_BLINK_COUNT; i++) {
        gpio_set_level(USER_LED_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(LED_BLINK_HALF_MS));
        gpio_set_level(USER_LED_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(LED_BLINK_HALF_MS));
    }
}

static void user_led_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << USER_LED_PIN,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(USER_LED_PIN, 0);
}

// The XIAO ESP32C6's USB-C port is wired straight to the chip's native USB, so
// the COM port tools/set_time.py opens is the USB-Serial-JTAG CDC device, not
// UART0. The console config makes UART0 primary and USB-Serial-JTAG only the
// *secondary* console, and a secondary console is output-only — host bytes
// arriving over USB are never delivered to the UART driver. So read the
// USB-Serial-JTAG peripheral directly.
//
// Installing this driver is safe alongside the secondary console: that path
// writes the TX FIFO polled (usb_serial_jtag_ll_write_txfifo) and we never call
// usb_serial_jtag_write_bytes(), so the driver's TX ring stays empty and ESP_LOG
// output is untouched. We take the driver purely for its interrupt-driven RX,
// which gives usb_serial_jtag_read_bytes() a real blocking timeout.
static void serial_driver_init(void)
{
    // tx_buffer_size must be > 0 even though we only ever read.
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
}

static uint8_t sum_bytes(const uint8_t *buf, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) sum = (uint8_t)(sum + buf[i]);
    return sum;
}

// Blocks indefinitely for one byte. Only used while hunting for the magic
// bytes, where there is nothing to time out on — the task simply sleeps in the
// USB-Serial-JTAG driver until the host sends something.
static uint8_t read_byte(void)
{
    uint8_t b;
    while (usb_serial_jtag_read_bytes(&b, 1, portMAX_DELAY) != 1) {
        // portMAX_DELAY only returns short on error; retry.
    }
    return b;
}

// Reads exactly `len` bytes, bounded by an overall deadline (esp_timer
// microseconds). Returns false on timeout.
static bool read_exact(uint8_t *buf, size_t len, int64_t deadline_us)
{
    size_t got = 0;
    while (got < len) {
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) return false;

        TickType_t ticks = pdMS_TO_TICKS(remaining_us / 1000);
        if (ticks == 0) ticks = 1;

        int n = usb_serial_jtag_read_bytes(buf + got, len - got, ticks);
        if (n <= 0) return false;
        got += (size_t)n;
    }
    return true;
}

// Reads one frame body (everything after the magic bytes), validates it, and
// applies it to the system clock. Returns true iff the clock was set.
static bool read_and_apply_frame(void)
{
    int64_t deadline_us = esp_timer_get_time() + (int64_t)FRAME_BODY_TIMEOUT_MS * 1000;

    time_sync_frame_t frame;
    frame.magic[0] = FRAME_MAGIC0;
    frame.magic[1] = FRAME_MAGIC1;

    uint8_t *body = (uint8_t *)&frame + offsetof(time_sync_frame_t, unix_time_s);
    size_t body_len = FRAME_SIZE - offsetof(time_sync_frame_t, unix_time_s) - sizeof(frame.checksum);
    if (!read_exact(body, body_len, deadline_us) ||
        !read_exact(&frame.checksum, sizeof(frame.checksum), deadline_us)) {
        ESP_LOGW(TAG, "Time-sync frame truncated - ignoring");
        return false;
    }

    uint8_t expected = sum_bytes((uint8_t *)&frame, FRAME_SIZE - sizeof(frame.checksum));
    if (frame.checksum != expected) {
        ESP_LOGW(TAG, "Time-sync checksum mismatch (got 0x%02x, want 0x%02x) - ignoring",
                 frame.checksum, expected);
        return false;
    }

    // Set the clock before tzset(). A POSIX TZ rule's DST transitions are
    // year-dependent, so installing the rule while the clock still reads 1970
    // has newlib compute the BST switchover dates for the wrong year.
    struct timeval tv = { .tv_sec = (time_t)frame.unix_time_s, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    frame.tz[TIME_SYNC_TZ_LEN - 1] = '\0'; // defensive, in case the host sent a full field
    if (frame.tz[0] != '\0') {
        setenv("TZ", frame.tz, 1);
        tzset();
    }

    time_t now = (time_t)frame.unix_time_s;
    struct tm t;
    localtime_r(&now, &t);
    ESP_LOGI(TAG, "System clock set from serial command: %04d-%02d-%02d %02d:%02d:%02d (TZ=%s)",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
             frame.tz[0] ? frame.tz : "unchanged");
    return true;
}

// Reads and validates the 2-byte body of a real-time-streaming command.
// Returns true iff the command was accepted; `*enabled` then holds its value.
static bool read_rt_cmd(bool *enabled)
{
    int64_t deadline_us = esp_timer_get_time() + (int64_t)FRAME_BODY_TIMEOUT_MS * 1000;

    uint8_t body[RT_CMD_BODY];
    if (!read_exact(body, sizeof(body), deadline_us)) {
        ESP_LOGW(TAG, "RT-stream frame truncated - ignoring");
        return false;
    }

    uint8_t expected = (uint8_t)(FRAME_MAGIC0 + RT_CMD_MAGIC1 + body[0]);
    if (body[1] != expected) {
        ESP_LOGW(TAG, "RT-stream checksum mismatch (got 0x%02x, want 0x%02x) - ignoring",
                 body[1], expected);
        return false;
    }

    *enabled = body[0] != 0;
    ESP_LOGI(TAG, "Real-time streaming command received: %s", *enabled ? "enable" : "disable");
    return true;
}

static void time_sync_task(void *arg)
{
    (void)arg;

    for (;;) {
        // Hunt for the magic bytes one at a time so stray bytes on the console
        // (a terminal's own keystrokes, a half-sent frame) don't desync the
        // parse. The second byte selects which command follows.
        if (read_byte() != FRAME_MAGIC0) continue;

        uint8_t kind = read_byte();

        if (kind == RT_CMD_MAGIC1) {
            bool enabled = false;
            if (!read_rt_cmd(&enabled)) continue;
            user_led_blink();
            if (s_on_rt_cmd) s_on_rt_cmd(enabled);
            continue;
        }

        if (kind != FRAME_MAGIC1) continue;

        if (!read_and_apply_frame()) continue;

        user_led_blink();
        if (s_on_sync) s_on_sync();
        xSemaphoreGive(s_synced);
    }
}

void time_sync_set_rt_stream_cb(time_sync_rt_cb_t cb)
{
    s_on_rt_cmd = cb;
}

void time_sync_start(time_sync_cb_t on_sync)
{
    if (s_synced) return;  // already started

    s_on_sync = on_sync;
    s_synced  = xSemaphoreCreateBinary();

    user_led_init();
    serial_driver_init();

    xTaskCreate(time_sync_task, "time_sync", 4096, NULL, 5, NULL);
}

bool time_sync_wait_for_command(uint32_t timeout_ms)
{
    if (!s_synced) {
        ESP_LOGE(TAG, "time_sync_start() not called - cannot wait for a time-sync command");
        return false;
    }

    ESP_LOGI(TAG, "Waiting up to %lu ms for a time-sync command "
                  "(run tools/set_time.py on the host)...",
             (unsigned long)timeout_ms);

    if (xSemaphoreTake(s_synced, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "Time-sync command timed out - system clock not set "
                      "(still listening; a later sync will be applied)");
        return false;
    }
    return true;
}
