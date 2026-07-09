#include "config.h"
#include "sensors.h"
#include "logger.h"
#include "display.h"
#include "wifi_ntp.h"
#include "file_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
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

// ── Download-mode button (GPIO20, falling edge) ──────────────────────────────
// Wired to a momentary button to GND (internal pull-up enabled). On press:
// stop recording and flush the EDF/log files, tear down BLE to free its heap,
// then bring up the Wi-Fi AP + HTTP file server so the SD card contents can
// be pulled off without opening the case.
#define DOWNLOAD_BTN_PIN GPIO_NUM_20

// Recording duration after which the display is shut down (§ wake button below).
#define DISPLAY_SLEEP_AFTER_S (20 * 60)

static QueueHandle_t s_download_evt_queue = NULL;

static void IRAM_ATTR download_btn_isr(void *arg)
{
    (void)arg;
    BaseType_t hpw = pdFALSE;
    uint32_t dummy = 0;
    xQueueSendFromISR(s_download_evt_queue, &dummy, &hpw);
    if (hpw) portYIELD_FROM_ISR();
}

static void download_mode_task(void *arg)
{
    (void)arg;
    uint32_t dummy;
    for (;;) {
        if (xQueueReceive(s_download_evt_queue, &dummy, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Download button pressed: stopping recording, "
                          "tearing down BLE, starting file server");
            logger_close();
            ble_client_deinit();
            file_server_start();
        }
    }
}

static void download_button_init(void)
{
    s_download_evt_queue = xQueueCreate(4, sizeof(uint32_t));

    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = 1ULL << DOWNLOAD_BTN_PIN;
    io_conf.mode         = GPIO_MODE_INPUT;
    io_conf.pull_up_en   = GPIO_PULLUP_ENABLE;
    io_conf.intr_type    = GPIO_INTR_NEGEDGE;
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(DOWNLOAD_BTN_PIN, download_btn_isr, NULL);

    xTaskCreate(download_mode_task, "dl_mode", 4096, NULL, 5, NULL);
}

// ── Display wake button (GPIO13, falling edge) ───────────────────────────────
// Wired to a momentary button to GND (internal pull-up enabled), same wiring
// as DOWNLOAD_BTN_PIN. Re-enables the display after display_sleep() has shut
// it down 20 minutes into a recording (see DISPLAY_SLEEP_AFTER_S below).
#define WAKE_BTN_PIN GPIO_NUM_13

static QueueHandle_t s_wake_evt_queue = NULL;

static void IRAM_ATTR wake_btn_isr(void *arg)
{
    (void)arg;
    BaseType_t hpw = pdFALSE;
    uint32_t dummy = 0;
    xQueueSendFromISR(s_wake_evt_queue, &dummy, &hpw);
    if (hpw) portYIELD_FROM_ISR();
}

static void wake_button_task(void *arg)
{
    (void)arg;
    uint32_t dummy;
    for (;;) {
        if (xQueueReceive(s_wake_evt_queue, &dummy, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Wake button pressed: re-enabling display");
            display_wake();
        }
    }
}

static void wake_button_init(void)
{
    s_wake_evt_queue = xQueueCreate(4, sizeof(uint32_t));

    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = 1ULL << WAKE_BTN_PIN;
    io_conf.mode         = GPIO_MODE_INPUT;
    io_conf.pull_up_en   = GPIO_PULLUP_ENABLE;
    io_conf.intr_type    = GPIO_INTR_NEGEDGE;
    gpio_config(&io_conf);

    // gpio_install_isr_service() is already called once in download_button_init().
    gpio_isr_handler_add(WAKE_BTN_PIN, wake_btn_isr, NULL);

    xTaskCreate(wake_button_task, "wake_btn", 4096, NULL, 5, NULL);
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
    int tick_rtc  = 0; // RTC drift-correction watchdog (in 50 Hz ticks)
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

                // 20 minutes into a recording, shut the display down
                // completely (backlight off, panel asleep, no more draws)
                // until the GPIO13 wake button is pressed. display_sleep()
                // is idempotent, so this just no-ops on every tick after
                // the first.
                if (logger_is_active() &&
                    logger_get_elapsed_seconds() >= DISPLAY_SLEEP_AFTER_S) {
                    display_sleep();
                }

                if (!display_is_sleeping()) {
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
                    dd.recording_seconds = logger_get_elapsed_seconds();
                    dd.wifi_ssid       = wifi_ntp_get_ssid();
                    dd.ap_active       = file_server_is_active();
                    display_update(&dd);
                }
            }

            // Periodic RTC drift correction (no-op if no RTC was detected)
            if (++tick_rtc >= (int)(RTC_SYNC_INTERVAL_MS / 20)) {
                tick_rtc = 0;
                sensors_rtc_periodic_sync();
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

    // If a DS1307 RTC is fitted and already holds a plausible time (i.e. it
    // has been set on a previous boot), use it and skip Wi-Fi entirely.
    // Otherwise fall back to Wi-Fi/NTP and, if an RTC is fitted, seed it so
    // future boots don't need Wi-Fi at all.
    if (!sensors_rtc_startup_sync()) {
        wifi_ntp_sync();
        sensors_rtc_write_from_system();
    }

    // SD card + EDF file (adds SD to the SPI bus display_init already created)
    if (!logger_init()) {
        ESP_LOGE(TAG, "Logger init failed (no SD card?) - continuing without recording");
    }

    // BLE client (NimBLE, runs its own FreeRTOS task internally)
    ble_client_init();

    // Download-mode button: stop recording / BLE, start Wi-Fi file server
    download_button_init();

    // Display wake button: re-enable the screen after display_sleep()
    wake_button_init();

    // Sensor loop
    xTaskCreate(sensor_task, "sensor", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "All tasks started");
}
