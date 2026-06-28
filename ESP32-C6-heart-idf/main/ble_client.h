#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialise NimBLE and start scanning. Non-blocking — BLE runs its own task.
void ble_client_init(void);

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
