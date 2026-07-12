#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Frame sent by the host, little-endian, no padding:
//   uint8_t  magic[2]     0xA5, 0x5A
//   int64_t  unix_time_s  seconds since Unix epoch, UTC
//   char     tz[40]       POSIX TZ string, NUL-terminated, zero-padded;
//                          empty (tz[0] == '\0') leaves TZ untouched
//   uint8_t  checksum     sum of all preceding bytes, truncated to uint8_t
#define TIME_SYNC_TZ_LEN 40

// Waits up to timeout_ms for a valid time-sync frame on the console UART
// (115200 baud, the same port used for flashing/monitoring) and, if one
// arrives, applies it to the system clock via settimeofday(). Replaces the
// old Wi-Fi/NTP sync: the host runs tools/set_time.py to send the frame.
// Returns true iff the system clock was set.
bool time_sync_wait_for_command(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
