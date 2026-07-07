#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Connect to Wi-Fi and synchronise the system clock from NTP.
// Blocks for up to 15 seconds. Logs a warning if NTP sync times out.
void wifi_ntp_sync(void);

// SSID of the network wifi_ntp_sync() last connected to, or "" if it either
// hasn't run yet or failed to connect to any configured candidate. Wi-Fi is
// torn down again once the one-shot sync completes, so this is a record of
// what was used at boot, not a live connection state.
const char *wifi_ntp_get_ssid(void);

#ifdef __cplusplus
}
#endif
