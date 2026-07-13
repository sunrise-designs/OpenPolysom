# Refactor Consistency and Bug Check

I've thoroughly reviewed the last two commits (`de80727b` and `aa02f169`), checking the new `time_sync`, `SH1106` display, and removed `BLE`/`WiFi` code. 

Overall, the structural changes are very solid (the I2C bus sharing is clean, SH1106 column offsets are correct, and the 100Hz ECG vs 50Hz logger multiplexing is mathematically flawless). However, I did find **one critical concurrency bug** (which is almost certainly the cause of the mysterious heap panic) and **a couple of timezone inconsistencies**.

## User Review Required

Please review the proposed fixes below. Once you approve, I will execute the changes.

> [!WARNING]
> **The Intermittent Heap Panic is a Concurrency Bug**
> In `main.cpp`, you commented out `logger_log_init()` because you suspected it was causing an intermittent heap panic. 
> The *actual* cause of the panic is a race condition between `sensor_task` and `download_mode_task`:
> 1. `sensor_task` calls `logger_record()`, which flushes data to the SD card. SD card writes (via SPI) are slow and cause the task to yield.
> 2. If the user presses the download button during this exact window, the `download_mode_task` preempts `sensor_task` and calls `logger_close()`.
> 3. `logger_close()` closes the EDF file and frees its memory (`edf_handle = -1`).
> 4. `sensor_task` resumes, attempts to finish its block write into a closed/freed file pointer, and causes a heap panic or memory corruption.
> 
> *Fix:* We need to introduce an `edf_mutex` in `logger.cpp` to synchronize `logger_record()` and `logger_close()`.

## Open Questions

None at this time.

## Proposed Changes

### Logger Component

#### [MODIFY] [logger.cpp](file:///c:/Repos/ProtoSom/ESP32-C6-heart-idf/components/logger/logger.cpp)
- **Fix Concurrency Bug:** Introduce a new `SemaphoreHandle_t edf_mutex`. Take this mutex inside `logger_record()`, `logger_record_ecg()`, and `logger_close()` to ensure the EDF file isn't closed while a write is in progress.
- **Fix Timezone Inconsistency:** The new serial time sync allows a PC to set a custom timezone, but `write_json_sidecar()` hardcodes `LOCAL_TZ`. Update it to dynamically read `getenv("TZ")` (falling back to `LOCAL_TZ` if null).

### Sensors Component

#### [MODIFY] [sensors.cpp](file:///c:/Repos/ProtoSom/ESP32-C6-heart-idf/components/sensors/sensors.cpp)
- **Fix Timezone Overwrite:** `sensors_rtc_periodic_sync()` temporarily switches the system to UTC to parse the RTC time, but then blindly restores the timezone to `LOCAL_TZ`. If the serial sync provided a custom TZ (e.g., `EST5EDT`), the RTC sync destroys it. Update `apply_utc_tm_to_system_clock()` to save and restore the *current* `getenv("TZ")` instead of hardcoding `LOCAL_TZ`.

### Main Application

#### [MODIFY] [main.cpp](file:///c:/Repos/ProtoSom/ESP32-C6-heart-idf/main/main.cpp)
- **Re-enable Logging:** Since we've identified the true cause of the heap panic, it is safe to re-enable `logger_log_init()` to ensure boot diagnostics are captured to the SD card.

## Verification Plan

### Manual Verification
- After applying these changes, I will ask you to build the firmware and deploy it.
- Try pressing the download button rapidly while recording to verify the heap panic no longer occurs.
- Verify that a custom timezone sent via `tools/set_time.py` persists in the JSON sidecar and system clock.
