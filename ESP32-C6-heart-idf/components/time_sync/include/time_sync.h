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

// Invoked from the time-sync task after a valid frame has been applied to the
// system clock (and after the user LED has flashed). Runs on the time-sync
// task, not the caller's — keep it short and don't assume the sensor loop is
// running yet, since the first sync normally lands during app_main().
typedef void (*time_sync_cb_t)(void);

// Installs the interrupt-driven USB-Serial-JTAG driver and starts a background
// task that listens for time-sync frames for the lifetime of the firmware, so
// the host can re-sync the clock at any point, not just during boot. Each
// accepted frame sets the system clock via settimeofday(), flashes the user
// LED, then calls `on_sync` (may be NULL).
//
// Call once, before time_sync_wait_for_command().
void time_sync_start(time_sync_cb_t on_sync);

// Blocks up to timeout_ms for the task above to accept a frame, so boot can
// hold off on opening the timestamped EDF file until the clock is right.
// Returns true iff a frame arrived within the timeout. A frame accepted before
// this call (or between calls) is remembered, so no sync is ever missed.
bool time_sync_wait_for_command(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
