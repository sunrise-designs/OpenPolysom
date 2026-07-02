#include "config.h"
#include "sensors.h"
#include "logger.h"
#include "display.h"
#include "wifi_ntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include <stdio.h>

extern "C" {
#include "ble_client.h"
}

static const char *TAG = "main";

// ── Boot diagnostics ──────────────────────────────────────────────────────────
// Survives every reset except power loss, so it counts unattended reboots.
static RTC_DATA_ATTR uint32_t s_boot_count = 0;

// Shown on the LCD because the resets under investigation only happen when no
// serial monitor is attached.
static void show_reset_reason(void)
{
    const char *rr;
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   rr = "POWER-ON";   break;
        case ESP_RST_SW:        rr = "SW-RESET";   break;
        case ESP_RST_PANIC:     rr = "PANIC";      break;
        case ESP_RST_INT_WDT:   rr = "INT-WDT";    break;
        case ESP_RST_TASK_WDT:  rr = "TASK-WDT";   break;
        case ESP_RST_WDT:       rr = "OTHER-WDT";  break;
        case ESP_RST_DEEPSLEEP: rr = "SLEEP-WAKE"; break;
        case ESP_RST_BROWNOUT:  rr = "BROWNOUT";   break;
        default:                rr = "OTHER";      break;
    }
    s_boot_count++;
    char msg[32];
    snprintf(msg, sizeof(msg), "RST:%s BOOT#%lu", rr, (unsigned long)s_boot_count);
    display_boot_msg(msg);
    ESP_LOGI(TAG, "Reset reason: %s, boot count %lu", rr, (unsigned long)s_boot_count);
}

// ── Deep-sleep entry ──────────────────────────────────────────────────────────
static void enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "Entering deep sleep for 10 minutes");
    logger_close();
    esp_sleep_enable_timer_wakeup(SLEEP_DURATION_US);
    esp_deep_sleep_start();
}

// ── Sensor / logger / display task (runs at 100 Hz) ──────────────────────────
static void sensor_task(void *arg)
{
    (void)arg;

    TickType_t       wake       = xTaskGetTickCount();
    const TickType_t period_ms  = pdMS_TO_TICKS(10);  // 100 Hz base tick

    int tick50   = 0;  // divides to 50 Hz
    int tick_disp = 0; // divides to 5 Hz
    int tick_scan = 0; // BLE rescan watchdog (in 50 Hz ticks)
    int scan_attempts = 0;

    while (1) {
        vTaskDelayUntil(&wake, period_ms);

        // 100 Hz: ECG ADC
        sensors_read_ecg();

        // 50 Hz: all other sensors + logging + display
        if (++tick50 >= 2) {
            tick50 = 0;

            sensors_read();

            logger_record(
                g_accel0_x, g_accel0_y, g_accel0_z,
                g_accel1_x, g_accel1_y, g_accel1_z,
                g_ldc0, g_ldc1,
                g_ecg_raw, g_pressure_mbar,
                ble_get_rr_ms());

            // 5 Hz: display
            if (++tick_disp >= 10) {
                tick_disp = 0;
                display_data_t dd = {};
                dd.ble_connected   = ble_is_connected();
                dd.ble_device_name = ble_get_device_name();
                dd.bpm             = ble_get_bpm();
                dd.rr_ms           = ble_get_rr_ms();
                dd.accel0[0]       = g_accel0_x;
                dd.accel0[1]       = g_accel0_y;
                dd.accel0[2]       = g_accel0_z;
                dd.accel1[0]       = g_accel1_x;
                dd.accel1[1]       = g_accel1_y;
                dd.accel1[2]       = g_accel1_z;
                dd.ldc0            = g_ldc0;
                dd.ldc1            = g_ldc1;
                dd.ldc0_baseline   = logger_get_ldc0_baseline();
                dd.ldc1_baseline   = logger_get_ldc1_baseline();
                dd.baseline_ok     = logger_get_baseline_ok();
                dd.recording       = logger_is_active();
                display_update(&dd);
            }

            // BLE rescan watchdog: restart scan every BLE_RESCAN_MS if not connected
            tick_scan++;
            if (!ble_is_connected() &&
                tick_scan >= (int)(BLE_RESCAN_MS / 20)) {
                tick_scan = 0;
                scan_attempts++;
                ESP_LOGI(TAG, "BLE rescan attempt %d", scan_attempts);
                if (scan_attempts >= MAX_SCAN_RETRIES) {
                    enter_deep_sleep();
                }
            } else if (ble_is_connected()) {
                scan_attempts = 0;
                tick_scan     = 0;
            }
        }
    }
}

// ── app_main ──────────────────────────────────────────────────────────────────
extern "C" void app_main(void)
{
    // Capture ESP_LOG output to SD as early as possible, before anything else logs.
    // Temporarily disabled while tracking down the intermittent BLE heap panic —
    // logger_log_init() installs the vprintf hook; leaving it uncalled means
    // log_mutex stays NULL, so every other logger_* call becomes a no-op
    // (they all guard on `if (!log_mutex) return;`). Re-enable once confirmed
    // innocent.
    // logger_log_init();

    // NVS required by Wi-Fi and NimBLE
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_LOGI(TAG, "Polysom ESP-IDF starting");

    // Display first (also initialises the shared SPI bus)
    display_init();
    show_reset_reason();

    // Sensors (I2C bus + ADC)
    sensors_init();

    // NTP time sync (Wi-Fi, then disconnects)
    wifi_ntp_sync();

    // SD card + EDF file (adds SD to the SPI bus display_init already created)
    if (!logger_init()) {
        ESP_LOGE(TAG, "Logger init failed (no SD card?) - continuing without recording");
    }

    // BLE client (NimBLE, runs its own FreeRTOS task internally)
    ble_client_init();

    // Sensor loop
    xTaskCreate(sensor_task, "sensor", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "All tasks started");
}
