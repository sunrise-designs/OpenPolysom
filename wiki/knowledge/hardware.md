---
title: Hardware & C++ Ingest
domain: knowledge
status: living
updated: 2026-07-08
summary: The acquisition devices (RPi5 + ESP32-S3 wrist unit), their sensors, exact EDF+ channel layouts, and the I2C/BLE data paths that feed the C++ ingest side.
---

# Hardware & C++ Ingest

The two acquisition devices that sit at the head of the [pipeline](../knowledge/architecture.md):
**device → ingest → signal-processing → web app**. Both are owned by Dmitry and both write
**EDF+** via [edflib](https://gitlab.com/Teuniz/EDFlib) — the **raw anchor** for biosignals
(see [data formats](../knowledge/data-formats.md)). EDF+ is the device output; **C++ ingest**
then converts it into the **raw Zarr** of the **working store**. Everything on this page is the
C++ ingest side of the [three-language boundary](../state/decisions.md) (C++ ingests, Python
processes, the TS web app presents; they meet at the **Zarr boundary**).

---

## 1. RPi5 acquisition unit — 6-channel EDF+

`src/main.cpp` runs on a Raspberry Pi 5. It is the main bedside unit, polling sensors on the
**I2C bus `/dev/i2c-1`** at 50 Hz and writing a 6-signal EDF+ file (`edfopen_file_writeonly(...,
EDFLIB_FILETYPE_EDFPLUS, 6)`, `src/main.cpp:126`). The EDF+ equipment field is stamped
`OpenPolysom v0.1 (<git-commit>)` for provenance (`src/main.cpp:34`).

Samples are written **physically** (`edfwrite_physical_samples`, `src/main.cpp:286–291`), so the
raw Zarr arrays produced from this file carry the attr `storage=physical`.

Channel table (exact, from `src/main.cpp:145–152`):

| # | Label | Transducer | Rate | Phys. dim | Phys. min/max | Digital min/max |
|---|-------|-----------|------|-----------|---------------|-----------------|
| 0 | `Thoracic` | LDC1612 CH0 — thoracic RIP belt | 50 Hz | Inductance (nH) | −1,000,000 / 1,000,000 | −32768 / 32767 |
| 1 | `Abdomen`  | LDC1612 CH1 — abdomen RIP belt  | 50 Hz | Inductance (nH) | −1,000,000 / 1,000,000 | −32768 / 32767 |
| 2 | `HR`       | Polar H9 via ESP32-S3 | 1 Hz | BPM | 0 / 250 | −32768 / 32767 |
| 3 | `RR`       | Polar H9 via ESP32-S3 | 5 Hz | ms | 0 / 2000 | −32768 / 32767 |
| 4 | `Flow`     | Sensirion SDP800-125P | 50 Hz | Pressure (mbar; EDF header label `Pressure`) | 0 / 1000 | −32768 / 32767 |
| 5 | `HR_Raw`   | AD8232 (ECG analog front-end) | 100 Hz | ADC | 0 / 4095 | 0 / 4095 |

Notes on the loop (`src/main.cpp:204–307`):

- **RIP belts (Thoracic, Abdomen).** Read from the LDC1612 inductance-to-digital converter
  (`get_channel_result`). The chest/abdomen belts modulate inductance with breathing. The loop
  averages the first second of raw counts at the 10-minute mark to set a **baseline**, then writes
  `raw − baseline` so the EDF+ value is signed deviation in nH (`src/main.cpp:252–268`). It writes
  before re-calibration too, accepting baseline drift over no data.
- **HR / RR** are pulled from the ESP32-S3 over I2C (`HrI2c::getLatestHR`); `Flow` from the SDP800
  (mbar); `HR_Raw` is the raw AD8232 ECG ADC stream drained into a 100-sample/record buffer
  (`hrI2c.drain(...)`, `src/main.cpp:284`).
- One EDF+ data record per second; flushed to disk every 10 s (`FLUSH_INTERVAL_SAMPLES`,
  `src/main.cpp:23`).

### Sensors on the RPi5

- **LDC1612** (Seeed) — dual-channel inductance-to-digital converter on I2C; verified by
  manufacturer ID `0x5449` (`src/main.cpp:18,99`). Drives the two **RIP** (respiratory inductance
  plethysmography) belts.
- **SDP800-125P** (Sensirion) — differential pressure sensor for nasal/oral **airflow**; degrades
  gracefully (`Flow` → zeros) if init/read fails (`src/main.cpp:113–116, 215–222`).
- **AD8232** (Analog Devices) — single-lead **ECG analog front-end**; its ADC stream is read by a
  helper (`hr_i2c.h`) and stored as `HR_Raw` for downstream R-peak / HRV work in
  [Python processing](../knowledge/signal-processing.md). [Datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/ad8232.pdf).

> [!warning] RIP physical-range quantization — open issue
> Thoracic/Abdomen declare a physical range of **±1,000,000 nH** but EDF+ stores a 16-bit digital
> sample (65,536 levels). The breathing signal is a tiny modulation around baseline, so the real
> swing occupies a sliver of that range — the LSB is ~30 nH and almost all 16 bits are wasted on
> headroom that never fills. Tightening `EDF_PHYS_MIN/MAX` (`src/main.cpp:27–28`) to the realistic
> post-baseline swing would recover effective resolution. Tracked in [decisions](../state/decisions.md).

---

## 2. ESP32-S3 wrist device — 4-channel EDF+

A Seeed XIAO ESP32-S3 worn on the wrist, sampling a single analog accelerometer and relaying the
Polar H9 heart-rate strap. Per Dmitry's design note (`src_python/readme.md:1–3`): the ESP32-S3 reads
HR from the **Polar H9** (easier to wear than wired ECG) and samples one **ADXL335** analog
accelerometer, then reports everything over I2C so the **RPi5 reads it on the shared I2C bus in its
own time**.

Two distinct data products come off this device:

1. **Self-contained EDF+ log on flash** (`logger.cpp`) — for standalone wrist recording.
2. **Live I2C feed to the RPi5** (`.ino` `onRequest`) — which supplies the RPi5's `HR`/`RR` channels.

### On-device EDF+ log (`logger.cpp` / `logger.h`)

4-signal EDF+ written to LittleFS (`/littlefs/biometric.edf`). 10-second data records;
**2160 records = 6 hours**, sized to fit the 1.5 MB LittleFS partition (`logger.h:5–13`).
Samples are written **digitally** (`edfwrite_digital_samples`, `logger.cpp:120–123`), so raw Zarr
arrays from this file carry the attr `storage=digital`.

| # | Label | Rate | Phys. dim | Phys. min/max | Digital min/max | Source |
|---|-------|------|-----------|---------------|-----------------|--------|
| 0 | `AccelX` | 10 Hz | mV | 0 / 3300 | 0 / 4095 | ADXL335 X (12-bit ADC) |
| 1 | `AccelY` | 10 Hz | mV | 0 / 3300 | 0 / 4095 | ADXL335 Y (12-bit ADC) |
| 2 | `AccelZ` | 10 Hz | mV | 0 / 3300 | 0 / 4095 | ADXL335 Z (12-bit ADC) |
| 3 | `RR`     | 1 Hz  | ms | 0 / 2000 | 0 / 2000 | Polar H9 RR interval |

(`logger.cpp:45–79`.) Note the **on-device accel rate is 10 Hz** in the logged EDF+ (the value
passed to `logRecord` every `LOG_RATE_MS = 100 ms`, `.ino:126–128`), even though the ADC itself is
sampled at 50 Hz in the loop (`ACCEL_RATE_HZ`, `.ino:16`). The start time comes from an NTP-synced
clock (`logger.cpp:82–87`). A `D` serial command hex-dumps the file; `E` erases and restarts
(`logger.cpp:138–168`).

### Acquisition loop & sensors (`BLE_HR_plus_accel_ADC.ino`)

- **ADXL335** (Analog Devices) — analog 3-axis accelerometer, read on GPIO2/3/4 (`ADC1_CH1/2/3`) at
  12-bit resolution, 50 Hz in-loop (`.ino:11–14, 64–98`). Captures body position / limb movement
  for PLM/LM scoring in [Python processing](../knowledge/signal-processing.md).
- **AD8232** ECG front-end is also wired (GPIO1 / `ADC1_CH0`) and sampled at 100 Hz on this device
  (`.ino:11, 86–90`), available over I2C as the ECG word — the wired-ECG option Dmitry flagged as
  harder to wear (`src_python/readme.md:7–8`). Electrodes: Black=RA, Blue=LA, Red=RL
  (`src_python/readme.md:25–33`).

### Data paths

**BLE (central → Polar H9).** The ESP32-S3 is a **BLE central/client**. It scans for and connects to
the Polar H9, subscribing to the standard **Heart Rate Service `0x180D`** / **Heart Rate Measurement
`0x2A37`** (`ble.cpp:5–6`). The notify callback parses the BLE flags byte, reads any RR-interval
fields, and converts the BT-SIG **1/1024 s** units to milliseconds: `rr_ms = rr_raw * 1000 / 1024`
(`ble.cpp:39–47`). BLE needs ≥80 MHz CPU clock; the chip runs at 80 MHz for radio-stack stability
(`.ino:52–53`). If the strap can't be found it **deep-sleeps for 10 minutes** then retries
(`.ino:19, 39–46, 115–123`).

**I2C (ESP32-S3 slave → RPi5 master).** The ESP32-S3 is an **I2C slave at address `0x30`**
(`.ino:8, 58`). On each RPi5 read it returns a fixed **10-byte big-endian** frame
(`onRequest`, `.ino:142–161`):

```
[ECG_H, ECG_L, X_H, X_L, Y_H, Y_L, Z_H, Z_L, RR_H, RR_L]
```

The RPi5 (`HrI2c` in `main.cpp`) polls this and routes HR → channel 2, RR → channel 3, and the ECG
word feeds `HR_Raw`. The two devices therefore **share one I2C bus**: the LDC1612/SDP800 are also on
`/dev/i2c-1`, and the ESP32-S3 joins as another slave.

---

## 3. Provenance & the Zarr boundary

Both devices emit **EDF+** as the immutable **raw anchor**. C++ ingest (Dmitry) reads that EDF+ via
edflib, converts each dense signal into a chunked **raw Zarr** array (each channel its own array,
chunked along time) plus extracted header metadata, and writes the **working store**. The
`storage=physical|digital` attr records which `edfwrite_*` path produced the source samples, so
[Python processing](../knowledge/signal-processing.md) and the TS web app can apply the EDF+
physical/digital scaling correctly. From there Python reads raw Zarr → writes the **derived layer**
+ `events.json` + `meta.json`; the [TS web app](../knowledge/viewer.md) only **reads** Zarr. See
[architecture](../knowledge/architecture.md) and [data formats](../knowledge/data-formats.md).

---

## 4. Future hardware direction

- **OpenPolysom** is the project name already stamped into the EDF+ equipment field
  (`src/main.cpp:34`); the RPi5 unit is its first reference build.
- **ESP32-C6** is the watch-item successor to the S3 wrist unit (lower power, native 802.15.4 /
  improved BLE + Wi-Fi 6) — a candidate for a smaller, longer-running wearable. Treat as an
  [open fork](../state/decisions.md), not a committed change.
- **24-bit EEG** would move biosignals to **BDF+** (the raw anchor format already anticipated for
  this — see [data formats](../knowledge/data-formats.md)).
- A wired-ECG path (AD8232 with full RA/LA/RL leads) remains an option Dmitry has explicitly noted
  as higher-fidelity but harder to wear than the Polar H9 strap (`src_python/readme.md:7–8`).
