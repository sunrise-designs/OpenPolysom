#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// This component is the device's serial command server. It began as (and is
// still named after) time sync, which remains its main job; a second command
// has since been added, and both share the one listener task.
//
// Time-sync frame, little-endian, no padding (tools/set_time.py):
//   uint8_t  magic[2]     0xA5, 0x5A
//   int64_t  unix_time_s  seconds since Unix epoch, UTC
//   char     tz[40]       POSIX TZ string, NUL-terminated, zero-padded;
//                          empty (tz[0] == '\0') leaves TZ untouched
//   uint8_t  checksum     sum of all preceding bytes, truncated to uint8_t
#define TIME_SYNC_TZ_LEN 40

// Real-time-streaming frame (tools/set_rt_stream.py). A distinct second magic
// byte rather than a type field inside the existing frame, so an older
// set_time.py keeps working byte-for-byte:
//   uint8_t  magic[2]     0xA5, 0x5B
//   uint8_t  enabled      0 = off, non-zero = on
//   uint8_t  checksum     sum of all preceding bytes, truncated to uint8_t

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

// Invoked from the same listener task when a real-time-streaming command frame
// is accepted. A callback rather than a direct call into components/rt_stream,
// so this component stays a serial reader that knows nothing about what the
// commands mean. Register before or after time_sync_start(); may be NULL.
typedef void (*time_sync_rt_cb_t)(bool enabled);
void time_sync_set_rt_stream_cb(time_sync_rt_cb_t cb);

// Blocks up to timeout_ms for the task above to accept a frame, so boot can
// hold off on opening the timestamped EDF file until the clock is right.
// Returns true iff a frame arrived within the timeout. A frame accepted before
// this call (or between calls) is remembered, so no sync is ever missed.
bool time_sync_wait_for_command(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
