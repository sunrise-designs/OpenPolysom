---
title: Hardware & C++ Ingest
domain: knowledge
status: living
updated: 2026-08-20
summary: The acquisition device (the ESP32-C6 unit), its sensors, the exact 11-channel EDF+ layout, the I2C data paths that feed the C++ ingest side, and the opt-in Wi-Fi rt_stream component that serves those same samples live.
---

# Hardware & C++ Ingest

The acquisition device that sits at the head of the [pipeline](../knowledge/architecture.md):
**device → ingest → signal-processing → web app**. It is owned by Dmitry and writes
**EDF+** via [edflib](https://gitlab.com/Teuniz/EDFlib) — the **raw anchor** for biosignals
(see [data formats](../knowledge/data-formats.md)). EDF+ is the device output; **C++ ingest**
then converts it into the **raw Zarr** of the **working store**. Everything on this page is the
C++ ingest side of the [three-language boundary](../state/decisions.md) (C++ ingests, Python
processes, the TS web app presents; they meet at the **Zarr boundary**).

> **The hardware runs entirely on the ESP32-C6.** Its 11-channel EDF+ is the **only** raw anchor
> the pipeline targets.

---

## 1. The ESP32-C6 unit — 11-channel EDF+

A [Seeed Studio XIAO ESP32-C6](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/) on a
bespoke breadboard (a KiCad PCB is in progress — see the `Hardware/` folder). The firmware
([`ESP32-C6-heart-idf`](../../ESP32-C6-heart-idf/)) keeps the radios off for a normal night's
recording, to cut power consumption. **BLE is not compiled in at all**; Wi-Fi is compiled in but
**off unless real-time streaming has been enabled for the device** (§ [live streaming](#3-live-sample-streaming--the-rt_stream-component)
below, and [decisions § S12](../state/decisions.md)):

- **Time sync** — a custom serial protocol (`tools/set_time.py`) instead of NTP, backed by a
  **DS3231** RTC on the I2C bus.
- **Display** — an **SH1106** I2C OLED for local status.
- **Data offload** — the Wi-Fi File Access Point was removed; EDF+ logs are read directly off the
  SD card. Live *streaming* is a different feature and does not offload files.

Samples are written **digitally** (`edfwrite_digital_samples`,
[`logger.cpp:548`](../../ESP32-C6-heart-idf/components/logger/logger.cpp)), so the raw Zarr arrays
produced from this file carry the attr `storage=digital`. Data records are **10 s**
(`RECORD_DURATION_S`, [`logger.h:9`](../../ESP32-C6-heart-idf/components/logger/include/logger.h)),
set via `edf_set_datarecord_duration` at [`logger.cpp:469`](../../ESP32-C6-heart-idf/components/logger/logger.cpp).

Channel table (exact, from the `SigDef` table at
[`logger.cpp:473–485`](../../ESP32-C6-heart-idf/components/logger/logger.cpp)):

| # | Label | Transducer | Rate | Phys. dim | Digital min/max | Phys. min/max |
|---|-------|-----------|------|-----------|-----------------|---------------|
| 0 | `Thoracic` | LDC1612 CH0 — thoracic RIP belt | 50 Hz | `counts` | −32767 / 32767 | −1,000,000 / 1,000,000 |
| 1 | `Abdomen`  | LDC1612 CH1 — abdomen RIP belt  | 50 Hz | `counts` | −32767 / 32767 | −1,000,000 / 1,000,000 |
| 2 | `Flow`     | SDP800-125Pa | 50 Hz | `mbar` | −32767 / 32767 | −100 / 100 |
| 3 | `ECG`      | AD8232 ADC0 | 100 Hz | `ADC` | 0 / 4095 | 0 / 4095 |
| 4–6 | `Accel0X/Y/Z` | MMA8451 ch0 | 50 Hz | `mg` | −8192 / 8191 | −2000 / 2000 |
| 7–9 | `Accel1X/Y/Z` | MMA8451 ch1 | 50 Hz | `mg` | −8192 / 8191 | −2000 / 2000 |
| 10 | `RR`      | `N/A` — **no live source** | 2.5 Hz | `ms` | 0 / 2000 | 0 / 2000 |

> `edf_set_samplefrequency()` takes **samples per data record**, not Hz — the effective rate is
> `samplefrequency / record duration`. `logger.cpp:494` computes it as
> `lround(rate * RECORD_DURATION_S)`; the `lround` guards `RR`'s fractional **2.5 Hz**
> (25 samples/record) against float truncation landing on 24.

### Cardiac: ECG only — `RR` is a dead channel

The **AD8232** analog front-end is read directly off **ADC channel 0** and genuinely sampled at
100 Hz. It is the device's **sole cardiac source**.

The `RR` channel exists in the EDF+ layout but has **no live source** — its transducer is literally
`"N/A"` and it logs zeros. On-device R-peak detection is not implemented, so RR intervals (and
therefore [HRV](../knowledge/signal-processing.md)) have no genuine input from this device today.
Anything the [Python processing](../knowledge/signal-processing.md) computes from `RR` on a current
recording is computed from zeros. Wiring RR up means either detecting R-peaks on-device from the
ECG channel, or deriving them host-side in [Python processing](../knowledge/signal-processing.md)
from the 100 Hz `ECG` array — the latter fits the language boundary better and is the likelier path.

There is no `HR` channel on the ESP32-C6 at all — heart rate is a derived quantity, not a captured one.

### Sensors on the I2C bus

- **LDC1612** (Seeed) — dual-channel inductance-to-digital converter; drives the two **RIP**
  (respiratory inductance plethysmography) belts on CH0/CH1.
- **SDP800-125Pa** (Sensirion) — differential pressure sensor for nasal/oral **airflow**, ±125 Pa.
- **MMA8451 ×2** (NXP) — two 3-axis accelerometers at different I2C addresses, reported in **mg**
  at 50 Hz, giving the bilateral (two-leg) limb-movement channels the AASM PLM scoring needs.
- **DS3231** — battery-backed RTC, so the recording start datetime survives a power cycle.
- **SH1106** — I2C OLED for local status.
- **AD8232** (Analog Devices) — single-lead **ECG analog front-end**, read via ADC (not I2C).
  [Datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/ad8232.pdf).

> [!warning] RIP physical-range quantization — open issue
> Thoracic/Abdomen declare a physical range of **±1,000,000 nH** against a 16-bit digital sample
> (65,536 levels), so the LSB is ~30 nH and almost all 16 bits are headroom that never fills.
> The firmware works around this **on the digital side** rather than by fixing the header: raw
> LDC1612 counts are divided by a per-channel constant (`THORACIC_DIVISOR 15`,
> `ABDOMEN_DIVISOR 11`, [`logger.cpp:194–195`](../../ESP32-C6-heart-idf/components/logger/logger.cpp))
> so each channel's breathing swing occupies more of the digital range. Those divisors were derived
> from **one night's** peak non-clipped swing with ~2× headroom — deeper breathing on a different
> patient or night could still saturate, and needs re-tuning. The declared physical range is
> untouched, so the physical→digital map in the header no longer describes nanohenries. Tracked as
> open fork **O3** in [decisions](../state/decisions.md).

### RIP baseline capture and its reboot behaviour

`logger.cpp` writes Thoracic/Abdomen as `raw − baseline`, capturing the baseline **20 minutes**
into the recording (`BASELINE_DELAY_S = 1200`, [`logger.cpp:243`](../../ESP32-C6-heart-idf/components/logger/logger.cpp))
averaged over 1 s (`BASELINE_AVG_SAMPLES = 50` @ 50 Hz), once belt-donning settles. Before that
point samples are written against a zero baseline — raw counts, effectively uncalibrated.

That uncalibrated window used to repeat after **every** reset. Since 2026-07-19 the captured
baseline is mirrored into RTC memory (`RTC_NOINIT_ATTR` + a magic word) and restored whenever
`esp_reset_reason()` is anything other than `ESP_RST_POWERON` — a panic/WDT/brownout reboot
mid-night resumes calibrated immediately, because the belts did not move across the reset.
A power-on still captures fresh, since unplugging the device in practice means the belts came
off. `RTC_NOINIT_ATTR` is load-bearing: `.rtc.data` (`RTC_DATA_ATTR`) is a *loaded* segment the
bootloader re-initialises on every non-deep-sleep boot, so it would not survive a panic reset.

The choice is recorded per-recording in the JSON sidecar's `ldc_baseline` block
(`ch0`/`ch1`/`valid`/`reused`/`reset_reason`/`delay_s`) — the
[zarr schema spec § 6.5](../planning/zarr-schema-spec.md) flags this on-device transform as
provenance that must not be lost. `reused: true` means the recording inherited the previous boot's
baseline rather than capturing its own — an ingest-visible fact, since a restored baseline predates
the recording's own start time.

### The JSON sidecar the device writes

Alongside the `.edf`, `write_json_sidecar` ([`logger.cpp:351`](../../ESP32-C6-heart-idf/components/logger/logger.cpp))
emits a `.json` carrying `device_uid` (derived from the MAC), `recording_start_time` /
`recording_end_time`, `timezone`, a `sensors[]` presence + sample-rate list, and the `ldc_baseline`
block above. This is **device-level** provenance and is distinct from the `meta.json` that
[Python processing](../knowledge/signal-processing.md) writes into the working store; C++ ingest
should fold it into the latter.

> **No EDF+ header provenance, and no PII.** The C6 firmware sets
> **no equipment string, no patient name, and no birthdate** — there is no `edf_set_equipment` /
> `edf_set_patientname` / `edf_set_birthdate` call anywhere in the firmware, and no git hash is
> compiled in. Two consequences, pulling opposite ways: the raw anchor is **PII-free by
> construction**, which removes the "hashed anchor embeds a patient name" conflict entirely (see
> [privacy](../standards/privacy.md)); but firmware build provenance is **absent**, not merely
> degraded, so an EDF+ from this device cannot currently be traced to the build that produced it.
> Restoring a full-SHA + dirty-flag stamp is owed — see [zarr schema spec § 6.5](../planning/zarr-schema-spec.md).

---

## 2. Provenance & the Zarr boundary

The device emits **EDF+** as the immutable **raw anchor**. C++ ingest (Dmitry) reads that EDF+ via
edflib, converts each dense signal into a chunked **raw Zarr** array (each channel its own array,
chunked along time) plus extracted header metadata, and writes the **working store**. The
`storage=digital` attr records that `edfwrite_digital_samples` produced the source samples, so
[Python processing](../knowledge/signal-processing.md) and the TS web app apply the EDF+
physical/digital scaling correctly. From there Python reads raw Zarr → writes the **derived layer**
+ `events.json` + `meta.json`; the [TS web app](../knowledge/viewer.md) only **reads** Zarr. See
[architecture](../knowledge/architecture.md) and [data formats](../knowledge/data-formats.md).

With a single acquisition device there is **one clock**, so every channel in the recording shares
one time origin and no cross-device offset + skew model is needed.

---

## 3. Live sample streaming — the `rt_stream` component

[`components/rt_stream`](../../ESP32-C6-heart-idf/components/rt_stream/) serves the samples
`logger.cpp` writes to the EDF+ over a **Wi-Fi WebSocket**, so the signals can be watched while a
recording runs — the [viewer](viewer.md)'s RT mode. It exists because a bad montage (a lifted
electrode, an unplugged belt) is currently only discoverable the next morning.

**Why wireless, not the USB link that is already there.** The AD8232 puts electrodes on the
patient. A USB tether to a mains-powered host is a leakage-current path; a battery-powered device
streaming over its own access point is not. Galvanic isolation is the reason this feature reopened
the no-Wi-Fi choice rather than riding the existing serial command channel — see
[decisions § S12](../state/decisions.md).

- **Off by default, per-device opt-in.** An NVS flag, set over the same serial command server as
  time sync (`tools/set_rt_stream.py`, magic `0xA5 0x5B`), applied on the next boot. Changing it
  live would put radio TX current in the path of the SD writes. The radio also stops itself after
  `RT_STREAM_IDLE_TIMEOUT_S` (10 min) with no client. An unattended night therefore keeps exactly
  the old power profile.
- **SoftAP** `ProtoSom-<last 3 MAC bytes>`, WPA2 with a per-device password derived from the MAC.
  Both are shown on the OLED's row 7, alternating, since there is nowhere else to discover them.
  STA mode is a `config.h` option for a lab setup.
- **The sample path never blocks.** `rt_stream_push_50hz`/`_ecg` are called from `sensor_task`
  beside `logger_record*()` and do a zero-timeout `xQueueSend`: a full queue **drops the frame and
  counts it**. Recording integrity outranks streaming, so a stalled radio can cost the viewer a
  visible gap but never delays the card. A lower-priority task drains the queue, formats one JSON
  frame per 100 ms with `snprintf` into a static buffer (no heap in the send loop), and broadcasts
  it.
- **Streamed values are the recorded values.** Thoracic/abdomen/flow are not what the sensor
  reported — they are baseline-subtracted, divided and scaled on the way to the EDF+. Rather than
  re-derive them, `rt_stream` calls `logger_thoracic_digital()` / `logger_abdomen_digital()` /
  `logger_flow_digital()`, which `logger_record()` itself uses. One implementation, so a streamed
  data point is the same integer that reaches the card.
- **Build cost:** the Wi-Fi + HTTP stack takes the binary from ~373 KB to ~1.15 MB, still 63% free
  in the 3 MB `factory` partition. No partition changes were needed.

The wire format and the reader side are documented in [viewer § RT vs batch](viewer.md).

## 4. Future hardware direction

- **OpenPolysom** is the project name; the ESP32-C6 breadboard unit is its current reference
  build, with a KiCad PCB in progress (`Hardware/`).
- **RR from ECG** — the highest-value missing link, since HRV is a
  [UARS](../knowledge/concepts.md) signal and the channel is currently zeros (§1).
- **24-bit EEG** would move biosignals to **BDF+** (the raw anchor format already anticipated for
  this — see [data formats](../knowledge/data-formats.md)). The `Hardware/` folder already carries
  an `EEG.kicad_sch`.
- **Audio** — a microphone path (`Microphone.kicad_sch`) for the [FLAC snore
  sidecar](../knowledge/data-formats.md) is designed but not yet captured in firmware.
- A **full-lead wired ECG** path (AD8232 with RA/LA/RL) remains an option Dmitry has noted as
  higher-fidelity but harder to wear.
