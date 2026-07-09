#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// One free-heap sample taken at a GAP connect/disconnect event, for tracking
// down a suspected slow BLE-stack memory leak across repeated reconnects.
typedef struct {
    time_t   timestamp;
    bool     connected;      // true = connect event, false = disconnect event
    uint32_t free_heap;
    uint32_t min_free_heap;  // low-water mark since boot (esp_get_minimum_free_heap_size)
} ble_heap_event_t;

#define BLE_HEAP_LOG_MAX 64

// Initialise NimBLE and start scanning. Non-blocking — BLE runs its own task.
void ble_client_init(void);

// Fully stop and tear down the NimBLE host + BLE controller, freeing their
// heap-backed buffer pools (this board has no PSRAM). Call from a task other
// than the internal NimBLE host task, e.g. a button handler — never from
// within a GAP/GATT callback. After this, the getters below read as
// disconnected/zero; ble_client_init() would need to run again to resume.
void ble_client_deinit(void);

// Live data accessors (safe to call from any task)
uint16_t    ble_get_bpm(void);
uint16_t    ble_get_rr_ms(void);
bool        ble_is_connected(void);
const char *ble_get_device_name(void);
const char *ble_get_device_address(void);
uint16_t    ble_get_hr_svc_uuid(void);

// Copies up to max_out recorded heap events (oldest first) into out and
// returns how many were copied. Logging stops once BLE_HEAP_LOG_MAX events
// have been recorded for the session (older events are not overwritten),
// so the earliest reconnects — most useful for spotting a per-cycle leak —
// are always captured.
size_t ble_get_heap_log(ble_heap_event_t *out, size_t max_out);

#ifdef __cplusplus
}
#endif
