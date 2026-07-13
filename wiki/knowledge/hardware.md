---
title: Hardware & C++ Ingest
domain: knowledge
status: living
updated: 2026-07-12
summary: The acquisition devices (RPi5 + ESP32-C6 wrist unit), their sensors, exact EDF+ channel layouts, and the I2C/BLE data paths that feed the C++ ingest side.
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

---
title: Hardware & C++ Ingest
domain: knowledge
status: living
updated: 2026-07-12
summary: The acquisition devices (RPi5 + ESP32-C6 wrist unit), their sensors, exact EDF+ channel layouts, and the I2C/BLE data paths that feed the C++ ingest side.
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
| 2 | `HR`       | Polar H9 via ESP32-C6 | 1 Hz | BPM | 0 / 250 | −32768 / 32767 |
| 3 | `RR`       | Polar H9 via ESP32-C6 | 5 Hz | ms | 0 / 2000 | −32768 / 32767 |
| 4 | `Flow`     | Sensirion SDP800-125P | 50 Hz | Pressure (mbar; EDF header label `Pressure`) | 0 / 1000 | −32768 / 32767 |
| 5 | `HR_Raw`   | AD8232 (ECG analog front-end) | 100 Hz | ADC | 0 / 4095 | 0 / 4095 |

Notes on the loop (`src/main.cpp:204–307`):

- **RIP belts (Thoracic, Abdomen).** Read from the LDC1612 inductance-to-digital converter
  (`get_channel_result`). The chest/abdomen belts modulate inductance with breathing. The loop
  averages the first second of raw counts at the 10-minute mark to set a **baseline**, then writes
  `raw − baseline` so the EDF+ value is signed deviation in nH (`src/main.cpp:252–268`). It writes
  before re-calibration too, accepting baseline drift over no data.
- **HR / RR** are pulled from the ESP32-C6 over I2C (`HrI2c::getLatestHR`); `Flow` from the SDP800
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

## 2. ESP32-C6 wrist device — 11-channel EDF+

The **ESP32-C6** is a smaller, longer-running wearable. The C6 firmware
([`ESP32-C6-heart-idf`](../../ESP32-C6-heart-idf/)) does **not** use BLE or Wi-Fi to reduce power consumption:

- **Sensors:** HR/RR have no live source on this device for now (logged as zero), and the ECG channel is a direct analog AD8232 read off ADC channel 0, genuinely sampled at 100 Hz, rather than a BLE link to a chest strap. It also logs multiple accelerometers (Accel0X/Y/Z, Accel1X/Y/Z).
- **Time Sync:** Syncs via a custom serial protocol (`tools/set_time.py`) instead of NTP.
- **Display:** Includes an SH1106 I2C OLED display for local status.
- **Data Offload:** The File Access Point (Wi-Fi server) was removed; 11-channel EDF+ logs must be read directly from the SD card.
- **I2C:** The RPi5 can poll it over I2C.

Samples are written **digitally** (`edfwrite_digital_samples`, `logger.cpp`), so raw Zarr
arrays from this file carry the attr `storage=digital`.

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
- **24-bit EEG** would move biosignals to **BDF+** (the raw anchor format already anticipated for
  this — see [data formats](../knowledge/data-formats.md)).
- A wired-ECG path (AD8232 with full RA/LA/RL leads) remains an option Dmitry has explicitly noted
  as higher-fidelity but harder to wear than the Polar H9 strap (`src_python/readme.md:7–8`).
