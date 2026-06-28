#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Connect to Wi-Fi and synchronise the system clock from NTP.
// Blocks for up to 15 seconds. Logs a warning if NTP sync times out.
void wifi_ntp_sync(void);

#ifdef __cplusplus
}
#endif
