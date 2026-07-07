#include "file_server.h"
#include "config.h"
#include "sdkconfig.h"
#include "display.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "file_server";

static bool s_started = false;

// ── Directory listing ─────────────────────────────────────────────────────────
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, "<html><body><h1>Polysom SD card</h1><ul>");

    DIR *dir = opendir(EDF_FILE_DIR);
    if (dir) {
        struct dirent *ent;
        char row[600];
        char path[300];
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;

            struct stat st = {};
            snprintf(path, sizeof(path), "%s/%s", EDF_FILE_DIR, ent->d_name);
            stat(path, &st);

            snprintf(row, sizeof(row), "<li><a href=\"/%s\">%s</a> (%ld KB)</li>",
                     ent->d_name, ent->d_name, (long)(st.st_size / 1024));
            httpd_resp_sendstr_chunk(req, row);
        }
        closedir(dir);
    } else {
        httpd_resp_sendstr_chunk(req, "<li>(could not open " EDF_FILE_DIR ")</li>");
    }

    httpd_resp_sendstr_chunk(req, "</ul></body></html>");
    httpd_resp_sendstr_chunk(req, NULL); // end of chunked response
    return ESP_OK;
}

// ── File download ─────────────────────────────────────────────────────────────
static const char *content_type_for(const char *filename)
{
    const char *ext = strrchr(filename, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".log")  == 0) return "text/plain";
    if (strcmp(ext, ".txt")  == 0) return "text/plain";
    if (strcmp(ext, ".edf")  == 0) return "application/x-edf";
    return "application/octet-stream";
}

static esp_err_t file_get_handler(httpd_req_t *req)
{
    // req->uri is "/<filename>" — decoded as-is, no sub-directories expected
    // since EDF_FILE_DIR is flat.
    char path[600];
    snprintf(path, sizeof(path), "%s%s", EDF_FILE_DIR, req->uri);

    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, content_type_for(req->uri));

    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0); // end of chunked response
    return ESP_OK;
}

static void start_httpd(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }

    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
    httpd_uri_t file = { .uri = "/*", .method = HTTP_GET, .handler = file_get_handler };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &file);
}

// ── Wi-Fi AP ──────────────────────────────────────────────────────────────────
static void start_ap(void)
{
    esp_err_t r = esp_netif_init();
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(r);

    r = esp_event_loop_create_default();
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(r);

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_cfg = {};
    strncpy((char *)ap_cfg.ap.ssid, CONFIG_POLYSOM_FILESERVER_AP_SSID,
            sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len       = strlen(CONFIG_POLYSOM_FILESERVER_AP_SSID);
    strncpy((char *)ap_cfg.ap.password, CONFIG_POLYSOM_FILESERVER_AP_PASSWORD,
            sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.max_connection = 2;
    ap_cfg.ap.authmode       = strlen(CONFIG_POLYSOM_FILESERVER_AP_PASSWORD) >= 8
                                    ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void file_server_start(void)
{
    if (s_started) {
        ESP_LOGW(TAG, "File server already running");
        return;
    }
    s_started = true;

    start_ap();
    start_httpd();

    ESP_LOGI(TAG, "File server ready: connect to Wi-Fi \"%s\" then browse http://192.168.4.1/",
             CONFIG_POLYSOM_FILESERVER_AP_SSID);

    char msg[48];
    snprintf(msg, sizeof(msg), "WiFi:%s IP:192.168.4.1", CONFIG_POLYSOM_FILESERVER_AP_SSID);
    display_boot_msg(msg);
}

bool file_server_is_active(void) { return s_started; }
