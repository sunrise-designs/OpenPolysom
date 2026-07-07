#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Brings up a Wi-Fi access point (SSID/password from Kconfig) and starts an
// HTTP server that lists and serves the contents of /sdcard for download.
// Logs the AP SSID/password and URL, and shows them on the LCD via
// display_boot_msg(). Safe to call more than once — later calls are no-ops.
//
// Only call this after BLE has been torn down (ble_client_deinit()) — this
// board has no PSRAM and Wi-Fi's driver buffer pools competing with NimBLE's
// heap-backed buffers is a known source of heap exhaustion here.
void file_server_start(void);

// True once file_server_start() has brought up the AP + HTTP server.
bool file_server_is_active(void);

#ifdef __cplusplus
}
#endif
