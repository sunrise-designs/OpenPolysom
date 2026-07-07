#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif
