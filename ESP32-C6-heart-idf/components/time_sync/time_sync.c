#include "time_sync.h"
#include "sdkconfig.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
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

static bool s_uart_driver_ready = false;

// The console UART defaults to non-blocking, ROM-polled I/O, which has no
// notion of a read timeout and would otherwise busy-spin waiting for bytes.
// Installing the interrupt-driven driver and handing the VFS console layer
// over to it (uart_vfs_dev_use_driver) gives uart_read_bytes() a real
// ticks_to_wait timeout while leaving stdout/ESP_LOG working as before.
static void ensure_uart_driver(void)
{
    if (s_uart_driver_ready) return;

    uart_vfs_dev_port_set_rx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_LF);
    uart_vfs_dev_port_set_tx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_CRLF);

    uart_config_t uart_config = {
        .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(CONFIG_ESP_CONSOLE_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0));
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);

    s_uart_driver_ready = true;
}

static uint8_t sum_bytes(const uint8_t *buf, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) sum = (uint8_t)(sum + buf[i]);
    return sum;
}

// Blocking read of exactly `len` bytes, bounded by an overall deadline
// (esp_timer microseconds). Returns false on timeout.
static bool read_exact(uint8_t *buf, size_t len, int64_t deadline_us)
{
    size_t got = 0;
    while (got < len) {
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) return false;

        TickType_t ticks = pdMS_TO_TICKS(remaining_us / 1000);
        if (ticks == 0) ticks = 1;

        int n = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, buf + got, len - got, ticks);
        if (n <= 0) return false;
        got += (size_t)n;
    }
    return true;
}

bool time_sync_wait_for_command(uint32_t timeout_ms)
{
    ensure_uart_driver();

    ESP_LOGI(TAG, "Waiting up to %lu ms for a time-sync command "
                  "(run tools/set_time.py on the host)...",
             (unsigned long)timeout_ms);

    int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    // Hunt for the two magic bytes one at a time so any stray bytes already
    // sitting in the RX buffer (e.g. a terminal's own keystrokes) don't
    // desync the frame parse.
    uint8_t b;
    for (;;) {
        if (!read_exact(&b, 1, deadline_us)) {
            ESP_LOGW(TAG, "Time-sync command timed out - system clock not set");
            return false;
        }
        if (b != FRAME_MAGIC0) continue;
        if (!read_exact(&b, 1, deadline_us)) {
            ESP_LOGW(TAG, "Time-sync command timed out - system clock not set");
            return false;
        }
        if (b == FRAME_MAGIC1) break;
    }

    time_sync_frame_t frame;
    frame.magic[0] = FRAME_MAGIC0;
    frame.magic[1] = FRAME_MAGIC1;

    uint8_t *body = (uint8_t *)&frame + offsetof(time_sync_frame_t, unix_time_s);
    size_t body_len = FRAME_SIZE - offsetof(time_sync_frame_t, unix_time_s) - sizeof(frame.checksum);
    if (!read_exact(body, body_len, deadline_us)) {
        ESP_LOGW(TAG, "Time-sync frame truncated - system clock not set");
        return false;
    }
    if (!read_exact(&frame.checksum, sizeof(frame.checksum), deadline_us)) {
        ESP_LOGW(TAG, "Time-sync frame truncated - system clock not set");
        return false;
    }

    uint8_t expected = sum_bytes((uint8_t *)&frame, FRAME_SIZE - sizeof(frame.checksum));
    if (frame.checksum != expected) {
        ESP_LOGW(TAG, "Time-sync checksum mismatch (got 0x%02x, want 0x%02x) - ignoring",
                 frame.checksum, expected);
        return false;
    }

    frame.tz[TIME_SYNC_TZ_LEN - 1] = '\0'; // defensive, in case the host sent a full field
    if (frame.tz[0] != '\0') {
        setenv("TZ", frame.tz, 1);
        tzset();
    }

    struct timeval tv = { .tv_sec = (time_t)frame.unix_time_s, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    time_t now = (time_t)frame.unix_time_s;
    struct tm t;
    localtime_r(&now, &t);
    ESP_LOGI(TAG, "System clock set from serial command: %04d-%02d-%02d %02d:%02d:%02d (TZ=%s)",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
             frame.tz[0] ? frame.tz : "unchanged");
    return true;
}
