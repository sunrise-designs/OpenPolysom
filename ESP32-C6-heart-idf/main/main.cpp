#include "config.h"
#include "sensors.h"
#include "battery_gauge.h"
#include "logger.h"
#include "display.h"
#include "time_sync.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include <stdio.h>

static const char *TAG = "main";

// ── Boot diagnostics ──────────────────────────────────────────────────────────
// Survives every reset except power loss, so it counts unattended reboots.
static RTC_DATA_ATTR uint32_t s_boot_count = 0;

// Which source set the system clock at boot ("RTC", "Serial", or "none") —
// shown on the display; see the RTC/time_sync block in app_main().
static const char *s_time_sync_source = "none";

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

// ── Serial time sync ─────────────────────────────────────────────────────────
// Called from the time_sync task on every accepted frame — at boot, and again
// whenever the host re-syncs mid-recording. The RTC write is what makes a
// mid-recording sync stick: sensor_task re-applies the RTC to the system clock
// every RTC_SYNC_INTERVAL_MS, which would otherwise undo the new time within
// ten minutes. The LED flash lives in the time_sync component itself.
static void on_time_sync(void)
{
    sensors_rtc_write_from_system();
    s_time_sync_source = "Serial";
}

// Recording duration after which the display is shut down (§ wake button below).
#define DISPLAY_SLEEP_AFTER_S (20 * 60)

// ── Display wake button (GPIO13, falling edge) ───────────────────────────────
// Wired to a momentary button to GND (internal pull-up enabled). Re-enables
// the display after display_sleep() has shut it down 20 minutes into a
// recording (see DISPLAY_SLEEP_AFTER_S below).
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

    gpio_install_isr_service(0);
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
    int tick_rtc  = 0; // RTC drift-correction watchdog (in 50 Hz ticks)
    bool auto_sleep_triggered = false;

    while (1) {
        vTaskDelayUntil(&wake, period_ms);

        // 100 Hz: ECG ADC (AD8232, ADC channel 0)
        sensors_read_ecg();
        logger_record_ecg(g_ecg_raw);

        // 50 Hz: all other sensors + logging + display
        if (++tick50 >= 2) {
            tick50 = 0;

            sensors_read();

            // BLE removed: HR/RR have no live source, logged as zero for now.
            logger_record(
                g_accel0_x, g_accel0_y, g_accel0_z,
                g_accel1_x, g_accel1_y, g_accel1_z,
                g_ldc0, g_ldc1,
                g_pressure_mbar,
                0);

            // 5 Hz: display
            if (++tick_disp >= 10) {
                tick_disp = 0;

                // 20 minutes into a recording, shut the display down
                // completely (backlight off, panel asleep, no more draws)
                if (logger_is_active() &&
                    logger_get_elapsed_seconds() >= DISPLAY_SLEEP_AFTER_S &&
                    !auto_sleep_triggered) {
                    display_sleep();
                    auto_sleep_triggered = true;
                }

                if (!display_is_sleeping()) {
                    display_data_t dd = {};
                    dd.accel0[0]       = g_accel0_x;
                    dd.accel0[1]       = g_accel0_y;
                    dd.accel0[2]       = g_accel0_z;
                    dd.accel1[0]       = g_accel1_x;
                    dd.accel1[1]       = g_accel1_y;
                    dd.accel1[2]       = g_accel1_z;
                    dd.ldc0            = g_ldc0;
                    dd.ldc1            = g_ldc1;
                    dd.pressure_mbar   = g_pressure_mbar;
                    dd.recording       = logger_is_active();
                    dd.recording_seconds = logger_get_elapsed_seconds();
                    dd.time_sync_source = s_time_sync_source;
                    dd.batt_percent    = battery_gauge_get_percentage();
                    display_update(&dd);
                }
            }

            // Periodic RTC drift correction (no-op if no RTC was detected)
            if (++tick_rtc >= (int)(RTC_SYNC_INTERVAL_MS / 20)) {
                tick_rtc = 0;
                sensors_rtc_periodic_sync();
            }
        }
    }
}

// ── app_main ──────────────────────────────────────────────────────────────────
extern "C" void app_main(void)
{
    // Capture ESP_LOG output to SD as early as possible, before anything else logs.
    // The previous intermittent heap panic was caused by an EDF write race condition,
    // which has now been fixed with `edf_mutex`. Re-enabling boot log capture.
    logger_log_init();

    ESP_LOGI(TAG, "Polysom ESP-IDF starting");

    // Display first (also initialises the shared I2C bus)
    display_init();
    show_reset_reason();

    // Sensors (register devices on the I2C bus display_init() created + ADC)
    if (!sensors_init(display_get_i2c_bus())) {
        ESP_LOGE(TAG, "Not all sensors detected - continuing anyway");
    }
    battery_gauge_init(sensors_get_adc_unit());

    // Listen for host time-sync commands on the console UART for the whole run,
    // not just at boot (see components/time_sync, tools/set_time.py), so the
    // clock can be corrected mid-recording. on_time_sync() seeds the RTC from
    // each accepted frame so future boots don't need the host.
    time_sync_start(on_time_sync);

    // If a DS3231 RTC is fitted and already holds a plausible time (i.e. it has
    // been set on a previous boot), use it and don't hold up boot waiting for a
    // host that may never send anything. The listener above stays up either way.
    if (sensors_rtc_startup_sync()) {
        s_time_sync_source = "RTC";
    } else {
        time_sync_wait_for_command(TIME_SYNC_TIMEOUT_MS);  // sets the source via on_time_sync()
    }

    // SD card + EDF file (logger owns the SD's SPI bus; display is I2C now)
    if (!logger_init()) {
        ESP_LOGE(TAG, "Logger init failed (no SD card?) - continuing without recording");
    }

    // Display wake button: re-enable the screen after display_sleep()
    // Disabled for now.
    // wake_button_init();

    // Sensor loop
    xTaskCreate(sensor_task, "sensor", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "All tasks started");
}
