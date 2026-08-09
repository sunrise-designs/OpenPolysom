#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Real-time sample streaming over Wi-Fi ────────────────────────────────────
//
// Serves the same samples logger.cpp writes to the EDF+ over a WebSocket, so a
// phone or laptop can watch the signals live while the recording runs. The
// EDF+ on the SD card remains the record of the night; this is only a view of
// it, and nothing here is persisted.
//
// Wi-Fi, not the USB-CDC link the host already uses for time sync, because the
// AD8232 puts electrodes on the patient: a USB tether to a mains-powered host
// is a leakage-current path, and a battery-powered device streaming wirelessly
// keeps the patient galvanically isolated. That is also why the default is a
// SoftAP — the device needs no infrastructure, and the live physiological data
// never reaches any other network.
//
// Wi-Fi costs power the rest of this firmware was written to save (see
// ../wiki/knowledge/hardware.md), so streaming is OFF unless explicitly enabled
// and shuts the radio down again once no client has been connected for
// RT_STREAM_IDLE_TIMEOUT_S. An unattended night recording keeps the old power
// profile untouched.
//
// The wire protocol is documented in ../wiki/knowledge/viewer.md § RT vs batch
// and implemented on the reader side in src_web/src/rt_protocol.ts.

// Bring up Wi-Fi and the WebSocket server, if streaming is enabled (see
// rt_stream_set_enabled / RT_STREAM_ENABLED_DEFAULT). No-op when disabled, and
// safe to call when no SD card / no recording is active. Call after
// logger_init(), so the first `hello` can report the real recording start.
void rt_stream_start(void);

// Feed the 50 Hz sample set — same arguments and same call site as
// logger_record(). NEVER blocks: if the send queue is full the frame is
// dropped and a counter is bumped. The recording must not be slowed down by a
// stalled radio.
void rt_stream_push_50hz(int16_t  a0x, int16_t  a0y, int16_t  a0z,
                         int16_t  a1x, int16_t  a1y, int16_t  a1z,
                         uint32_t ldc0, uint32_t ldc1,
                         float    pressure_mbar,
                         uint16_t rr_ms);

// Feed one 100 Hz ECG sample — same call site as logger_record_ecg().
// Never blocks, same as above.
void rt_stream_push_ecg(uint16_t ecg_raw);

// Enable/disable streaming and persist the choice in NVS, so it survives a
// reboot mid-study. Takes effect on the next boot (bringing Wi-Fi up or down
// mid-recording would risk the SD write path this firmware protects).
void rt_stream_set_enabled(bool enabled);

// Whether streaming is on for this boot.
bool rt_stream_is_enabled(void);

// Whether the radio is currently up (false once the idle timeout has stopped it).
bool rt_stream_is_up(void);

// Connected WebSocket clients right now.
uint32_t rt_stream_client_count(void);

// Sample frames discarded because the send queue was full, since boot. Non-zero
// means the viewer's picture has holes in it; the EDF+ does not.
uint32_t rt_stream_dropped(void);

// The SoftAP SSID and password in force, for display on the OLED. Both point at
// static storage and are valid after rt_stream_start(); the password is derived
// per-device from the MAC, so it is not a shared secret across units.
const char *rt_stream_ssid(void);
const char *rt_stream_password(void);

#ifdef __cplusplus
}
#endif
