#include "wifi_ntp.h"
#include "sdkconfig.h"
#include "config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "wifi_ntp";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRY     5

static EventGroupHandle_t s_wifi_events;
static int s_retry_count = 0;
static char s_connected_ssid[33] = "";

typedef struct {
    const char *ssid;
    const char *password;
} wifi_candidate_t;

static const wifi_candidate_t s_wifi_candidates[] = {
    { CONFIG_POLYSOM_WIFI_SSID,   CONFIG_POLYSOM_WIFI_PASSWORD },
    { CONFIG_POLYSOM_WIFI_SSID_2, CONFIG_POLYSOM_WIFI_PASSWORD_2 },
    { CONFIG_POLYSOM_WIFI_SSID_3, CONFIG_POLYSOM_WIFI_PASSWORD_3 },
};

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_MAX_RETRY) {
            s_retry_count++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

// Tries each configured Wi-Fi network in turn until one connects.
// Returns true once esp_wifi_start()/connect has succeeded for a candidate.
static bool wifi_connect_any(void)
{
    for (size_t i = 0; i < sizeof(s_wifi_candidates) / sizeof(s_wifi_candidates[0]); i++) {
        const wifi_candidate_t *candidate = &s_wifi_candidates[i];
        if (candidate->ssid[0] == '\0') {
            continue;
        }

        wifi_config_t wifi_cfg = {0};
        strncpy((char *)wifi_cfg.sta.ssid, candidate->ssid,
                sizeof(wifi_cfg.sta.ssid) - 1);
        strncpy((char *)wifi_cfg.sta.password, candidate->password,
                sizeof(wifi_cfg.sta.password) - 1);
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

        s_retry_count = 0;
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGI(TAG, "Connecting to %s (%u/%u)...", candidate->ssid,
                 (unsigned)(i + 1), (unsigned)(sizeof(s_wifi_candidates) / sizeof(s_wifi_candidates[0])));
        EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                                WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                                pdFALSE, pdFALSE,
                                                pdMS_TO_TICKS(10000));

        if (bits & WIFI_CONNECTED_BIT) {
            strncpy(s_connected_ssid, candidate->ssid, sizeof(s_connected_ssid) - 1);
            s_connected_ssid[sizeof(s_connected_ssid) - 1] = '\0';
            return true;
        }

        ESP_LOGW(TAG, "Failed to connect to %s", candidate->ssid);
        esp_wifi_stop();
    }

    return false;
}

void wifi_ntp_sync(void)
{
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h_any, h_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &h_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &h_ip));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    if (wifi_connect_any()) {
        ESP_LOGI(TAG, "Connected. Starting SNTP sync with %s", CONFIG_POLYSOM_NTP_SERVER);
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, CONFIG_POLYSOM_NTP_SERVER);
        esp_sntp_init();

        // SNTP sets the system clock to UTC; apply the local time zone rule so
        // localtime_r() (here and elsewhere, e.g. the EDF filename/header) reports
        // correct UK wall-clock time, including the BST offset in summer.
        setenv("TZ", LOCAL_TZ, 1);
        tzset();

        // Wait up to 10 s for time to be set
        time_t now = 0;
        struct tm t = {0};
        int retries = 0;
        while (t.tm_year < (2020 - 1900) && retries++ < 20) {
            vTaskDelay(pdMS_TO_TICKS(500));
            time(&now);
            localtime_r(&now, &t);
        }
        if (t.tm_year >= (2020 - 1900))
            ESP_LOGI(TAG, "Time synced: %04d-%02d-%02d %02d:%02d:%02d",
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                     t.tm_hour, t.tm_min, t.tm_sec);
        else
            ESP_LOGW(TAG, "NTP sync timed out");

        esp_sntp_stop();
    } else {
        ESP_LOGW(TAG, "Wi-Fi connection failed on all networks — clock not synced");
    }

    // Wi-Fi is only needed for this one-shot clock sync. esp_wifi_stop() alone
    // leaves the driver's TX/RX buffer pools resident on the heap for the rest
    // of the run, competing with BLE's heap-backed mbuf pool (no PSRAM on this
    // board — ~300KB internal RAM total) — fully deinit so that heap is back.
    esp_wifi_stop();
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, h_any);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, h_ip);
    esp_wifi_deinit();
    esp_netif_destroy_default_wifi(sta_netif);
    esp_event_loop_delete_default();
    vEventGroupDelete(s_wifi_events);
    ESP_LOGI(TAG, "Wi-Fi torn down");
}

const char *wifi_ntp_get_ssid(void)
{
    return s_connected_ssid;
}
