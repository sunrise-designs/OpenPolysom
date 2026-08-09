---
title: PSG-on-Zarr Schema Specification
domain: planning
status: snapshot
updated: 2026-07-19
summary: The concrete Zarr working-store + events + metadata schema and the EDF round-trip, rewritten 2026-07-19 against the single-device ESP32-C6 hardware.
---

> Note: the language split has since settled to **C++ ingest / Python processing / TS web app** (see [decisions](../state/decisions.md)). The Zarr schema, events, metadata, and round-trip below stand; treat any in-text language attributions as the design-time snapshot.
>
> **Revised 2026-07-19 for single-device hardware.** This spec was originally written against a two-device rig (RPi5 bedside unit + ESP32-C6). The RPi5 is retired; **the ESP32-C6's 11-channel EDF+ is the sole raw anchor**, and the tables below have been rewritten against `logger.cpp`'s actual `SigDef`. Three structural simplifications follow and are reflected throughout: there is now **one clock** (the cross-device offset/skew model in §6.4 collapses), **one `signals/` group** (the `signals_esp/` split is gone), and **every array is `storage="digital"`** (the firmware writes `edfwrite_digital_samples` exclusively, so the physical-vs-digital asymmetry that drove §3.2 no longer exists at capture — the attribute is retained for derived arrays and future producers).

# ProtoSom — PSG-on-Zarr Schema Specification (v0.1)

**Status:** proposal for Dmitry review. **Scope:** layer (2) of the three-layer data model — the unified working store, its events sidecar, its metadata/provenance manifest, and the EDF/BDF round-trip at the edges. **Audience:** the host-side data pipeline (Python) and Dmitry's host-side C++ tooling. The device firmware never writes Zarr.

---

## 1. Overview & the three-layer model

ProtoSom stores one sleep recording as a directory holding three layers. **Layer 1 (raw capture)** is whatever the device emits — EDF+ for biosignals, FLAC for audio — immutable and content-hashed; it is the provenance anchor and is never modified. **Layer 2 (unified working store)** is a single Zarr group per recording (every dense signal its own array, chunked along time), plus two JSON sidecars (`events.json` for sparse/typed annotations, `meta.json` for metadata + provenance + the 3-layer manifest); it is regenerable from layer 1 and is what the local slicing server reads and what the DSP reads/writes. **Layer 3 (clinical export)** is EDF+/BDF+ regenerated on demand for EDFBrowser interop; it is never stored. The on-disk shape:

```
2026-07-16_2300_protosom-c6/         # recording directory; recording_id = dir name
  meta.json                          # §6 — metadata + provenance + 3-layer manifest
  raw/
    biometric_2026-07-16_23-00-00.edf  # layer 1: immutable EDF+ (ESP32-C6, 11 ch), hashed
    biometric_2026-07-16_23-00-00.json # layer 1: the firmware's own device sidecar, hashed
    snore_16k.flac                   # layer 1: immutable FLAC (future), hashed + time-anchored
  working.zarr/                      # layer 2: one Zarr v2 group (§3)
  events.json                        # layer 2: sparse events/annotations (§5)
  export/
    clinical.edf                     # layer 3: regenerated, not committed (§7)
```

> **Reality check (state this to Dmitry up front).** The EDF→Zarr ingest and the Zarr→EDF export described here **do not exist yet**. Today's Python pipeline reads the EDF+ raw anchor directly, not via a raw Zarr; `export_zarr.py` writes single-chunk arrays with no calibration attributes. This spec defines the **target contract** and names what must be built (`ingest_edf.py`, `export_edf.py`, a DSP refactor). None of the round-trip or re-derivation guarantees are exercised by current code.

---

## 2. Zarr version + codec decision

**Decision: Zarr v2.** Rationale, tied to C++ library support:

- The binding constraint is the **intersection of the three C++ libraries the brief names**. The canonical [Zarr implementations registry](https://zarr.dev/implementations/) lists **z5 = v2 only, xtensor-zarr = v2 only, TensorStore = v2 + v3**. Choosing finalized Zarr v3 collapses the C++ reader set to **TensorStore alone** — exactly the single-library lock-in the brief flagged as the biggest risk.
- The "xtensor-zarr does v3" claim does not survive scrutiny: its last release (v0.0.7, 2021) implements the **abandoned pre-ZEP1 draft** (`.zr3` store, `meta/root/<name>.array.json`), whose layout is mutually unreadable with finalized v3's single `zarr.json` per node. The registry correctly classes it v2 for practical purposes.
- v3's only load-bearing advantage at our scale is `sharding_indexed` (avoids the "millions of tiny chunk files" problem). The brief's own design — server slices windows, pyramids added later — keeps per-night chunk counts modest (a 50 Hz channel over 8 h ≈ 480 chunks; see §3/§4 sizing), so sharding is **not needed for v0.1**. The v2→v3 migration is mechanical (re-encode arrays, same logical model), so this is a reversible bet; revisit if (a) per-night chunk-file counts cross ~10⁵ **and** (b) z5 is dropped as a candidate.

**Codec (every array, identical):** numcodecs **Blosc(cname=`zstd`, clevel=5, shuffle=`SHUFFLE`)** for integer arrays; `shuffle=BITSHUFFLE` for float arrays. Blosc+zstd+shuffle is the best ratio/speed combination for low-entropy physiological time-series and is implemented by all three C++ libs plus zarr-python. **`filters: null`** on every array (no numcodecs Delta/FixedScaleOffset/PackBits/Categorize — those are Python-only traps for the C++ readers; byte-shuffle gives the delta-like benefit portably). **`dimension_separator: "."`** pinned for maximum C++ reader compatibility. `shuffle` in a v2 `.zarray` is the **integer** `1` (SHUFFLE) / `2` (BITSHUFFLE), not the v3 string — a C++ writer must emit the integer.

---

## 3. Zarr group layout, dtypes, chunks, attributes

### 3.1 Worked tree (the ESP32-C6 11-channel recording + derived)

```
working.zarr/                       # .zgroup {"zarr_format": 2}; root .zattrs in §6.1
├─ signals/                         # device-captured biosignals (regenerable from raw EDF+)
│   ├─ thoracic     int16    rate 50   chunks (3000,)   storage=digital   mask=thoracic_mask
│   ├─ abdomen      int16    rate 50   chunks (3000,)   storage=digital   mask=abdomen_mask
│   ├─ flow         int16    rate 50   chunks (3000,)   storage=digital   mask=flow_mask
│   ├─ ecg          int16    rate 100  chunks (6000,)   storage=digital   mask=ecg_mask
│   ├─ accel0_x     int16    rate 50   chunks (3000,)   storage=digital   mask=accel0_x_mask
│   ├─ accel0_y     int16    rate 50   chunks (3000,)   storage=digital
│   ├─ accel0_z     int16    rate 50   chunks (3000,)   storage=digital
│   ├─ accel1_x     int16    rate 50   chunks (3000,)   storage=digital   mask=accel1_x_mask
│   ├─ accel1_y     int16    rate 50   chunks (3000,)   storage=digital
│   ├─ accel1_z     int16    rate 50   chunks (3000,)   storage=digital
│   └─ rr           int16    rate 2.5  chunks (1024,)   storage=digital   mask=rr_mask
├─ derived/
│   ├─ accel0_mag   float32  rate 50  chunks (3000,)   gap=NaN   (per-leg PLM input)
│   ├─ accel1_mag   float32  rate 50  chunks (3000,)   gap=NaN   (per-leg PLM input)
│   ├─ accel_combined_mag  float32  rate 50  chunks (3000,)  gap=NaN  (bilateral, headline)
│   ├─ hrv_rmssd    float32  irregular  time_array=derived/hrv_t   gap=NaN
│   ├─ hrv_t        float64  irregular  (seconds from t0)
│   └─ audio/
│       └─ snore_spectrogram  float32  2D [freq,time]  chunks (257, 512)
└─ pyramid/                         # multi-resolution; LAYOUT FIXED NOW, COMPUTED LATER (§4)
    ├─ thoracic/   1/ 2/ 3/ 4/ 5/   # each: int16 shape (n,2) last axis=[min,max]
    └─ ecg/        1/ 2/ ...
```

A single `signals/` group, not the old `signals/` + `signals_esp/` split: with one acquisition
device there is no second clock to segregate. All three accel magnitude traces are **derived**
(they are computed by [Python processing](../knowledge/signal-processing.md) from the six raw
axes), which is why the bilateral combined channel lives under `derived/` alongside the per-leg
ones — see [signal processing § 3](../knowledge/signal-processing.md).

### 3.2 Storage policy — the single normative table

The physical-vs-digital asymmetry the original reviewers caught was an artefact of two firmwares
disagreeing. With the RPi5 retired it is **gone**: `logger.cpp` writes **every** channel via
`edfwrite_digital_samples` (line 548), so the on-disk digital samples are exactly what the firmware
computed — no edflib requantization anywhere in the capture path. The per-array **`storage`**
attribute is retained regardless, because the *derived* layer holds calibrated float arrays and a
future producer may write physical doubles. This table is canonical; §5/§6/§7 reference it and do
not redefine dtypes.

Values are exact from the `SigDef` table at `logger.cpp:473–485`.

| Array | Source | Rate (Hz) | `storage` | Zarr dtype | digital_min/max | physical_min/max | unit | gap mechanism |
|---|---|---|---|---|---|---|---|---|
| `signals/thoracic` | LDC1612 CH0 | 50 | digital | int16 | −32767 / 32767 | −1e6 / 1e6 | `counts` (nominal nH — see O3) | mask array |
| `signals/abdomen` | LDC1612 CH1 | 50 | digital | int16 | −32767 / 32767 | −1e6 / 1e6 | `counts` (nominal nH — see O3) | mask array |
| `signals/flow` | SDP800-125Pa | 50 | digital | int16 | −32767 / 32767 | −100 / 100 | mbar | mask array |
| `signals/ecg` | AD8232 ADC0 | 100 | digital | int16 | 0 / 4095 | 0 / 4095 | ADC | mask array |
| `signals/accel0_x/y/z` | MMA8451 ch0 | 50 | digital | int16 | −8192 / 8191 | −2000 / 2000 | mg | mask array |
| `signals/accel1_x/y/z` | MMA8451 ch1 | 50 | digital | int16 | −8192 / 8191 | −2000 / 2000 | mg | mask array |
| `signals/rr` | **none — logs zeros** | 2.5 | digital | int16 | 0 / 2000 | 0 / 2000 | ms | mask array |
| **Future** `signals/eeg` (2D `[ch,time]`) | EEG ADC | 256 | digital | int32 | −8388608 / 8388607 | per montage | µV | mask array |
| **Future** `signals/spo2` | pulse-ox | 1 | digital | uint8 | 0 / 100 | 0 / 100 | % | mask array |
| **Future** `signals/body_position` | position | 1 | digital | uint8 | 0 / 5 | 0 / 5 | code | `categories` attr |

- **`storage="digital"`** (lossless int) is now the case for **every captured channel**: the
  integer is stored exactly and EDF↔Zarr↔EDF is bit-identical throughout. This is a strictly
  stronger round-trip guarantee than the original two-device design could offer.
- **`storage="physical"`** (float32 calibrated) survives only in `derived/`, where Python
  processing writes real units (mg magnitudes, ms RMSSD). Ingest never emits it today.
- **`signals/rr` carries no information.** The channel is declared with transducer `"N/A"` and the
  firmware writes zeros — there is no R-peak detection on the device. Ingest must **not** silently
  present it as a valid series; mark it fully masked (`rr_mask` all-1) or set an explicit
  `no_source: true` attr so `compute_hrv` cannot be fed zeros and report a confident RMSSD. See
  [hardware](../knowledge/hardware.md).
- **`accel0_*`/`accel1_*` are int16 in mg, not uint16 ADC counts.** The MMA8451 reports signed
  physical acceleration, `digital_min/max = −8192/8191` against `±2000 mg`. Any note claiming
  12-bit `0–4095` counts at `0–3300 mV` describes the retired ADXL335-on-RPi path and is wrong for
  this hardware — see the §8 DSP-domain note.
- **24-bit EEG → int32** (sign-extended), with `bit_depth: 24` and `digital_min/max: −8388608/8388607` (the 24-bit range, **not** the int32 range — the C++ BDF exporter needs this to pack 3-byte two's-complement little-endian words). Never store raw 3-byte packed samples.
- **Dtype subset is the portable intersection only:** `{int16, int32, uint8, uint16, float32, float64}`. No `object`, structured/record, string, or `datetime64` dtypes anywhere (Python-only traps).

> **Two raw data-quality facts to escalate to Dmitry, not silently absorb.** (1) The RIP physical range is still ±1e6 nH against 16-bit digital — an EDF quantum of 2e6/65535 ≈ **30.5 nH per LSB**. The firmware now compensates on the *digital* side, dividing raw LDC1612 counts by per-channel constants (`THORACIC_DIVISOR 15`, `ABDOMEN_DIVISOR 11`, `logger.cpp:194–195`) so the swing occupies more of the digital range, but it does **not** adjust the declared physical range to match. Net effect: resolution is recovered, and the header's affine map no longer produces nanohenries. The divisors were tuned on **one night's** peak swing with ~2× headroom and can still saturate. Both halves belong in provenance. Tracked as open fork **O3**. (2) `logger.cpp` subtracts a baseline from thoracic/abdomen *before* writing (captured at `BASELINE_DELAY_S = 1200`, averaged over `BASELINE_AVG_SAMPLES = 50`), so the EDF "raw" is **already baseline-subtracted** — and, since 2026-07-19, that baseline may have been **restored from RTC memory across a reboot**, meaning it can predate the recording's own start time. The firmware's JSON sidecar records this in its `ldc_baseline` block (`ch0`/`ch1`/`valid`/`reused`/`reset_reason`/`delay_s`); ingest must fold it into `provenance.on_device_transforms` rather than let it stop at the device.

### 3.3 Per-array attribute convention

Attributes live in the array's `.zattrs` (plain JSON only — cast every numpy scalar to native `float`/`int` before writing). All keys required unless marked optional. Example `signals/thoracic/.zattrs`:

```json
{
  "protosom_role": "signal",
  "storage": "digital",
  "label": "Thoracic",
  "edf_label": "Thoracic",
  "transducer": "LDC1612 CH0",
  "edf_physical_dimension": "counts",
  "harmonized_unit": "counts",
  "kind": "respiratory_effort",
  "sample_rate_hz": 50.0,
  "edf_digital_min": -32767, "edf_digital_max": 32767,
  "edf_physical_min": -1000000.0, "edf_physical_max": 1000000.0,
  "edf_record_duration_s": 10.0,
  "gain": 30.518043793, "offset": 0.0,
  "on_device_divisor": 15,
  "t0_offset_s": 0.0,
  "n_samples": 1620000,
  "mask_array": "thoracic_mask",
  "source_device": "esp32c6",
  "source_edf_signal_index": 0,
  "_ARRAY_DIMENSIONS": ["time"]
}
```

| Key | Meaning / contract |
|---|---|
| `protosom_role` | `signal` \| `derived` \| `pyramid` \| `mask`. Lets readers/validators branch without path parsing. |
| `storage` | `physical` \| `digital` (§3.2). The single most important attr: tells every reader and the exporter whether the array holds calibrated floats or raw ints. |
| `edf_label`, `transducer`, `edf_physical_dimension` | **Verbatim EDF header strings** — guarantee EDFBrowser-identical labels on export. `edf_physical_dimension` is the literal byte string (e.g. `"Pressure"` for flow, `"ADC"` for ecg). |
| `harmonized_unit` | Post-hoc corrected unit (e.g. `mbar` for flow). Never replaces `edf_physical_dimension`; both are kept so an auditor sees the divergence is deliberate. |
| `kind` | Controlled vocab (§6.3) the viewer/DSP key off, not the free-text label. |
| `sample_rate_hz` | The only per-array clock. With `t0` (root) + `t0_offset_s` + index, every sample has an absolute time (§3.4). |
| `edf_digital_min/max`, `edf_physical_min/max` | Verbatim from EDF. Source of truth for calibration: `gain=(pmax−pmin)/(dmax−dmin)`, `offset=pmin−gain·dmin`, `physical=gain·digital+offset`. (Worked: thoracic gain = 2e6/65534 = 30.5180…; offset = 0.0. Note the span is 65534, not 65536 — `logger.cpp` declares a **symmetric** ±32767 digital range, not −32768/32767.) `gain`/`offset` are stored as convenience; recompute from min/max on export to avoid float drift. |
| `on_device_divisor` | *(RIP channels only, optional.)* The `THORACIC_DIVISOR`/`ABDOMEN_DIVISOR` the firmware applied to raw counts before writing. Informational — it is **not** part of the EDF affine map, and it is why `physical` on these two channels is not nanohenries. Recording it is what lets an auditor reconstruct the true count scale. |
| `edf_record_duration_s` | EDF data-record duration of the source (**10.0** — `RECORD_DURATION_S`, set via `edf_set_datarecord_duration` at `logger.cpp:469`). Independent of the Zarr chunk length, which is chosen purely for slicing. Note that EDF's `samplefrequency` field is samples-*per-record*, so effective Hz = `samplefrequency / edf_record_duration_s`; this is where the `RR` channel's fractional 2.5 Hz lives (25 samples/record). |
| `fill_value_meaning` / mask ref | Gap convention (§3.5). |
| `_ARRAY_DIMENSIONS` | xarray convention, plain JSON string array (C++-safe): `["time"]`, or `["channel","time"]` (EEG), or `["freq","time"]` (spectrogram). |

### 3.4 Time & alignment

- **One shared origin per recording**, `t0_iso` (root `.zattrs`), stored ISO-8601 with explicit offset. **It is the whole-second EDF header start.** `logger.cpp:502` sets only whole-second `edf_set_startdatetime`, so any subsecond fraction is *not* in the raw header. For the PoC, `t0_iso` is whole-second; any subsecond origin is tracked separately and emitted as the `+0.X` first time-keeping TAL on export (edflib supports this once firmware passes a fractional start — it does not today, so the raw subsecond origin is already lost; flag to Dmitry). The device's own JSON sidecar carries `recording_start_time` plus an explicit `timezone`, which is the better source for the ISO offset than inferring one.
- **No global time axis, no per-sample timestamp arrays** for uniform signals. Sample `i` → `t0 + t0_offset_s + i / sample_rate_hz`. Time is `int64` Unix-ns / ISO in attrs; never a `datetime64` dtype. **Irregular series (HRV) carry a `float64` `*_t` companion** in seconds-from-t0 (float64, not float32 — at 28800 s a float32 step is ~0.004 s, too coarse for beat timing).
- **One device, one clock — cross-device alignment is no longer a problem.** With the RPi5 retired, every channel in a recording comes off the same ESP32-C6 timebase (serial time sync + a DS3231 RTC), so an event scored on one channel may freely reference any other. `source_device` and `t0_offset_s` are retained in the schema for forward-compatibility, but today `source_device` is uniformly `"esp32c6"` and `t0_offset_s` is uniformly `0.0`. The `offset_s + skew_ppm` model in §6.4 becomes a **single** device-to-UTC statement rather than a device-to-device reconciliation. If a second acquisition device is ever reintroduced, the drift caveat comes back with it and must be re-derived — a constant offset cannot correct skew over a night.

### 3.5 Gap / missing-data model

The signal-arrays slice's "fill_value = digital_min" scheme is broken against the actual firmware and is **dropped**. The firmware writes **0** on source failure, and `0` is a legal physiological/ADC value — so a real dropout is indistinguishable from a valid zero, and `digital_min` is in fact never written. The convention:

- **Every captured array is `storage="digital"` (int)**, so the gap mechanism is uniformly a companion **`uint8` mask array** (`<name>_mask`, `0`=valid, `1`=gap) — no in-band int sentinel is safe. Mask arrays carry `protosom_role: "mask"`. This is simpler than the original two-mechanism design, which needed masks for ints and NaN for floats depending on which firmware wrote the channel.
- **`storage="physical"` (float32) arrays** exist only in `derived/`. There the gap sentinel is **`NaN`** (JSON string `"NaN"` in `.zattrs` fill_value — never a bare token; a C++ writer must emit the literal string).
- **`signals/rr` is a whole-array gap.** It is not "sometimes missing" — it has no source at all and is zeros end to end. Its mask should be all-1 from ingest, so downstream code that respects masks cannot mistake it for a flat-but-valid series.
- The `events.json` `gap` annotation (§5) is an optional human-readable index, never the sole record of a gap.

---

## 4. Decimation-pyramid convention

The viewer asks the server for `(channel, window, target_px)`; the server decides the level. This contract is unchanged whether the pyramid is precomputed or the server decimates l0 on the fly — so v0.1 ships on-the-fly decimation and a build step fills `pyramid/` later **with zero viewer change**.

- **min/max pairs, interleaved in one array**, shape `(n, 2)` last axis `[min, max]` (2D EEG: `[channel, n, 2]`). Min/max preserves spikes/apneas that mean-decimation would hide; interleaving (vs separate `_min`/`_max` arrays) halves file count and makes a pixel's min–max band one contiguous read. **No LTTB in v0.1.** Pyramid arrays inherit the base array's dtype/calibration and add `decimation_factor`, `parent`, `protosom_role: "pyramid"`.
- **Factor 4 per level** (`l1=÷4, l2=÷16, …`), giving ~log₄(N) levels. Stop when a level's length ≤ 4096 (read whole anyway).
- **Pyramid chunk length = `clamp(round(base_chunk_len / factor), 2048, base_chunk_len)`**; any level ≤ 8192 samples is a single chunk. This avoids the micro-chunk inode-waste the naive `base/factor` rule hits at coarse levels.
- **Server level selection (arithmetic, no metadata scan):** for window `[t0,t1]`, viewport width `W`, required samples ≈ `(t1−t0)·rate`; pick the smallest factor `f` with `(t1−t0)·rate / f ≤ 2·W`; read `pyramid/<ch>/<level(f)>` over the window, or `signals/<ch>` if `f==1`. Between levels the server decimates the *chosen coarser level* to `target_px` — O(level samples), not O(l0).
- **Gap-aware decimation (part of the build contract):** exclude NaN / masked samples from a bin's min/max; if a whole bin is gap, emit NaN so the renderer draws nothing — otherwise gaps render as full-scale spikes exactly where data is missing.

**Pyramid depth, worked (8-hour night):**

| Array | rate | l0 samples | levels (factor 4) | top level |
|---|---|---|---|---|
| `thoracic` | 50 | 1.44M | l1..l5 (÷4..÷1024) | ~1406 samples |
| `accel0_x` (×6 accel axes) | 50 | 1.44M | l1..l5 (÷4..÷1024) | ~1406 samples |
| `ecg` | 100 | 2.88M | l1..l6 (÷4..÷4096) | ~703 samples |
| `eeg` (256 Hz, time axis only) | 256 | 7.37M | l1..l6 (÷4..÷4096) | ~1800 samples |

Note the accel axes moved from 10 Hz to **50 Hz** with the MMA8451, and there are now **six** of
them rather than three — 5× the rate across 2× the channels is a **10× increase** in accelerometry
volume over the original estimate. It is still small next to the spectrogram, but it is no longer
negligible against `thoracic`/`ecg`.

**Spectrogram (largest future array, must be spec'd concretely):** 8 h @ nfft=512/hop=256 ≈ 1.8M frames × 257 bins × f32 ≈ **1.85 GB**. Chunk `[all_freq=257, time_chunk≈512]` (~526 KB/chunk, in the ~1 MB sweet spot — **not** the 120 s rule, which would give 7.7 MB chunks). It gets its own **time-axis max/mean pyramid** like EEG so a whole-night overview is not a 1.85 GB scan. Put the decode budget in view: at ~5–10 GB/s Blosc-zstd a full-night scan is ~0.2–0.4 s, hence the overview must hit the pyramid, not l0.

---

## 5. Events / annotations JSON sidecar

**Rule:** dense & regularly-sampled → Zarr; sparse, ragged, or typed-with-provenance → `events.json`. JSON Schema **Draft 2020-12**, validatable from Python (`jsonschema`) and C++ (`nlohmann/json` + `pboettch/json-schema-validator`). No NaN/Inf (forbidden in JSON — use `null`); `onset_s`/`duration_s` are `double`.

**Key model decisions:** one general Event model (`onset_s, duration_s, type, channels, params, confidence`) covers LM, apnea, hypopnea, RERA, arousal, desaturation, **and** each 30-s hypnogram epoch (a stage is an event whose `duration_s` == epoch length). PLM series are a **Group** referencing member LM events by `id` (not nested `[[onset,offset]]` pairs — that shape is lossy). Multiple scorers coexist as `scorings[]` entries in one file. HRV is **not** here — it is a dense Zarr array (§3); only its overall scalar lives in `meta.json` stats. Time-base is `float64` seconds from `recording_start`, matching the EDF+ TAL onset convention so `onset_s` maps 1:1 to the export `+onset` field. **`event.channels[]` and `group`/`event` channel references use the canonical snake_case Zarr array names from §3.2** (e.g. `ecg`, `accel0_mag`, `accel_combined_mag`, future `eeg`), never EDF+ labels.

### 5.1 Schema (abridged to the load-bearing `$defs`)

```jsonc
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "protosom/events-1.0.0.json",
  "type": "object",
  "required": ["schema", "schema_version", "recording_id", "time_base", "scorings"],
  "properties": {
    "schema":         { "const": "protosom.events" },
    "schema_version": { "type": "string", "pattern": "^\\d+\\.\\d+\\.\\d+$" },
    "recording_id":   { "type": "string" },
    "time_base": {
      "type": "object", "required": ["reference","start_iso","unit"],
      "properties": {
        "reference": { "const": "recording_start" },
        "start_iso": { "type": "string", "format": "date-time" },  // == meta.json + Zarr t0 (whole-second)
        "unit":      { "const": "s" }
      }
    },
    "channels_ref": { "const": "working.zarr" },
    "scorings":     { "type": "array", "items": { "$ref": "#/$defs/scoring" } }
  },
  "$defs": {
    "scoring": {
      "type": "object",
      "required": ["scoring_id","scorer","scoring_method","provenance","events"],
      "properties": {
        "scoring_id":     { "type": "string" },
        "scorer":         { "type": "object", "required": ["kind","name"],
                            "properties": { "kind": {"enum":["human","algorithm"]},
                                            "name": {"type":"string"}, "version": {"type":"string"} } },
        "scoring_method": { "enum": ["auto","manual","hybrid"] },
        "hypopnea_rule":  { "enum": ["1A","1B"] },     // required when any hypopnea present
        "provenance":     { "$ref": "#/$defs/provenance" },
        "events":         { "type": "array", "items": { "$ref": "#/$defs/event" } },
        "groups":         { "type": "array", "items": { "$ref": "#/$defs/group" } }
      }
    },
    "event": {
      "type": "object",
      "required": ["id","type","onset_s","duration_s"],
      "properties": {
        "id":         { "type": "string" },           // unique within this scoring
        "type":       { "type": "string" },           // controlled vocab §5.2
        "onset_s":    { "type": "number", "minimum": 0 },
        "duration_s": { "type": "number", "minimum": 0 },
        "channels":   { "type": "array", "items": { "type": "string" } },  // canonical Zarr names
        "confidence": { "type": "number", "minimum": 0, "maximum": 1 },
        "respiratory_associated": { "type": "boolean" },  // LM excluded from PLMI per AASM if true
        "links":      { "type": "array", "items": {        // typed cross-event refs (hypopnea→arousal)
                          "type": "object", "required": ["rel","target_id"],
                          "properties": { "rel": {"type":"string"}, "target_id": {"type":"string"} } } },
        "params":     { "type": "object" }
      }
    },
    "group": {
      "type": "object",
      "required": ["id","type","member_ids"],
      "properties": {
        "id": {"type":"string"}, "type": {"type":"string"},
        "member_ids": { "type": "array", "items": {"type":"string"} },
        "onset_s": {"type":"number"}, "duration_s": {"type":"number"}, "params": {"type":"object"}
      }
    },
    "provenance": {
      "type": "object",
      "required": ["function","code_version","params","input_signals","raw_refs"],
      "properties": {
        "function":      { "type": "string" },
        "code_version":  { "type": "string" },     // full 40-char git SHA + ":dirty" suffix if dirty
        "params":        { "type": "object" },
        "source_constants": { "type": "object" },  // hardcoded constants, informational (see note)
        "input_signals": { "type": "array", "items": {"type":"string"} },
        "raw_refs":      { "type": "array", "items": {   // list, because a recording can have >1 raw EDF
                             "type": "object", "required": ["path","hash"],
                             "properties": { "path": {"type":"string"}, "hash": {"$ref":"#/$defs/hash"} } } }
      }
    }
  }
}
```

### 5.2 Controlled vocabulary (`type`)

| `type` | duration | scored on (canonical names) | notes |
|---|---|---|---|
| `limb_movement` | 0.5–10 s | `accel0_mag`, `accel1_mag`, `accel_combined_mag` | `respiratory_associated:true` ⇒ excluded from PLMI ([AASM](https://aasm.org/)) |
| `apnea_obstructive` / `_central` / `_mixed` | ≥10 s, ≥90% flow drop | `flow`,`thoracic`,`abdomen` | effort distinguishes subtype |
| `hypopnea` | ≥10 s, ≥30% flow drop + (desat or arousal) | `flow`,`spo2` | scoring `hypopnea_rule` 1A/1B required; `params.subtype: obstructive\|central` optional |
| `rera` | ≥10 s effort/flattening → arousal | `flow`,`eeg` | |
| `arousal` | ≥3 s EEG shift | `eeg`,`emg` | |
| `desaturation` | local ≥3% SpO2 drop | `spo2` | `params.nadir_pct`,`drop_pct` |
| `sleep_stage` | epoch (30 s) | `eeg`,`eog`,`emg` | `params.stage` ∈ `[W,N1,N2,N3,R,movement,unscored,artifact]` |
| `gap` | dropout span | the affected channel | optional human index of a §3.5 gap |

**Hypnogram invariant (validator-enforced):** `sleep_stage` events within one scoring tile `[lights_off_s, lights_on_s]` contiguously at `epoch_s`, no gaps/overlaps; `movement`/`unscored`/`artifact` are explicit (EDF+ `?` / `Movement time`). The scoring carries `lights_off_s`/`lights_on_s` so TST and sleep efficiency are computable.

### 5.3 TODAY example (LM/PLM; HRV deliberately absent)

```jsonc
{
  "schema": "protosom.events", "schema_version": "1.0.0",
  "recording_id": "2026-07-16_2300_protosom-c6",
  "time_base": { "reference": "recording_start", "start_iso": "2026-06-19T22:14:03", "unit": "s" },
  "channels_ref": "working.zarr",
  "scorings": [{
    "scoring_id": "plm-auto-v0",
    "scorer": { "kind": "algorithm", "name": "protosom.count_plm", "version": "0.3.1" },
    "scoring_method": "auto",
    "provenance": {
      "function": "signal_processing.count_plm",
      "code_version": "81955d3...e (full SHA)",
      "params": { "threshold": 8.0, "fs": 50, "max_threshold": 500.0, "tilt_threshold_deg": 10.0,
                  "window_sec": 30 },
      "source_constants": { "lm_min_dur_s": 0.5, "lm_max_dur_s": 10.0,
                            "plm_min_gap_s": 5, "plm_max_gap_s": 90, "plm_min_series": 4,
                            "min_gravity_mg": 100 },
      "input_signals": ["accel_combined_mag"],
      "raw_refs": [{ "path": "raw/biometric_2026-07-16_23-00-00.edf",
                     "hash": { "algorithm": "blake3", "value": "af3c…91" } }]
    },
    "events": [
      { "id":"ev_0001","type":"limb_movement","onset_s":1432.5,"duration_s":1.3,"channels":["accel_combined_mag"],"params":{"peak_vm":14.2} },
      { "id":"ev_0002","type":"limb_movement","onset_s":1454.9,"duration_s":0.9,"channels":["accel_combined_mag"],"params":{"peak_vm":11.0} },
      { "id":"ev_0003","type":"limb_movement","onset_s":1478.1,"duration_s":1.1,"channels":["accel_combined_mag"],"params":{"peak_vm":12.7} },
      { "id":"ev_0004","type":"limb_movement","onset_s":1500.4,"duration_s":1.0,"channels":["accel_combined_mag"],"params":{"peak_vm":13.1} }
    ],
    "groups": [
      { "id":"grp_plm_001","type":"plm_series","member_ids":["ev_0001","ev_0002","ev_0003","ev_0004"],
        "onset_s":1432.5,"duration_s":69.0,"params":{"n_movements":4,"mean_interval_s":22.6} }
    ]
  }]
}
```

> `peak_vm`, `n_movements`, `mean_interval_s`, and per-LM `id`s are **NOT produced by today's `count_plm`** (it emits anonymous `(on/fs, off/fs)` second-tuples). The migrator must assign deterministic ids before `member_ids` can reference them; the params are NEW fields requiring a `count_plm` extension, optional/absent in plain migration. `threshold:8` is on the **retired uint8 byte-log scale** and has never been recalibrated for the MMA8451's mg output — as are `max_threshold` and `tilt_threshold_deg`, tuned against a single recording. All three are uncalibrated placeholders; see the §8 note and open fork **O12** in [decisions](../state/decisions.md). Note there are now **three** scorings per recording (per-leg Accel0, per-leg Accel1, and the bilateral combined headline), not one.

### 5.4 FUTURE example (apnea + hypopnogram, two scorers)

```jsonc
{
  "schema": "protosom.events", "schema_version": "1.0.0",
  "recording_id": "2027-03-02_2300_protosom-full",
  "time_base": { "reference": "recording_start", "start_iso": "2027-03-02T23:00:00", "unit": "s" },
  "channels_ref": "working.zarr",
  "scorings": [
    {
      "scoring_id": "resp-auto-v1",
      "scorer": { "kind": "algorithm", "name": "protosom.detect_resp", "version": "1.2.0" },
      "scoring_method": "auto", "hypopnea_rule": "1A",
      "provenance": { "function": "signal_processing.detect_respiratory_events",
        "code_version": "deadbeef…(full)", "params": { "apnea_drop_pct": 90, "hypopnea_drop_pct": 30, "min_dur_s": 10, "desat_pct": 3 },
        "input_signals": ["flow","thoracic","abdomen","spo2"],
        "raw_refs": [{ "path":"raw/biosignals.bdf", "hash": {"algorithm":"blake3","value":"ffe…"} }] },
      "events": [
        { "id":"ev_a01","type":"apnea_obstructive","onset_s":4210.0,"duration_s":18.4,
          "channels":["flow","thoracic","abdomen"],"params":{"flow_drop_pct":96,"paradoxical":true},"confidence":0.88 },
        { "id":"ev_h01","type":"hypopnea","onset_s":4905.2,"duration_s":14.0,"channels":["flow","spo2"],
          "params":{"flow_drop_pct":42,"desat_pct":4.1,"subtype":"obstructive"},
          "links":[{"rel":"caused_arousal","target_id":"ev_ar01"}],"confidence":0.74 },
        { "id":"ev_d01","type":"desaturation","onset_s":4912.0,"duration_s":22.0,"channels":["spo2"],"params":{"nadir_pct":89.5,"drop_pct":4.1} },
        { "id":"ev_ar01","type":"arousal","onset_s":4923.0,"duration_s":4.2,"channels":["eeg"] }
      ]
    },
    {
      "scoring_id": "hypnogram-auto-v1",
      "scorer": { "kind": "algorithm", "name": "protosom.stage_sleep", "version": "1.0.0" },
      "scoring_method": "auto", "lights_off_s": 0.0, "lights_on_s": 28800.0,
      "provenance": { "function": "signal_processing.stage_sleep", "code_version": "deadbeef…(full)",
        "params": { "epoch_s": 30, "model": "yasa-like-v1" },
        "input_signals": ["eeg","eog","emg"], "raw_refs": [{ "path":"raw/biosignals.bdf", "hash": {"algorithm":"blake3","value":"ffe…"} }] },
      "events": [
        { "id":"ep_0001","type":"sleep_stage","onset_s":0.0,  "duration_s":30.0,"params":{"stage":"W"} },
        { "id":"ep_0002","type":"sleep_stage","onset_s":30.0, "duration_s":30.0,"params":{"stage":"N1"} },
        { "id":"ep_0003","type":"sleep_stage","onset_s":60.0, "duration_s":30.0,"params":{"stage":"N2"} },
        { "id":"ep_0004","type":"sleep_stage","onset_s":90.0, "duration_s":30.0,"params":{"stage":"N3"} }
        /* … one per 30-s epoch for the whole night (~960 events; fine as JSON) … */
      ]
    }
  ]
}
```

---

## 6. Metadata + provenance + 3-layer manifest (`meta.json`)

Metadata lives in a standalone `meta.json` at the recording root, **not** in Zarr `.zattrs` — it is C++-readable with zero Zarr dependency and must describe layers (raw EDF+/FLAC) that exist outside Zarr. The Zarr root `.zattrs` carries only a back-pointer. JSON Schema Draft 2020-12.

### 6.1 Root `.zattrs` (back-pointer only)

```json
{ "protosom_meta": "meta.json", "protosom_schema_version": "0.1.0",
  "recording_id": "2026-07-16_2300_protosom-c6", "t0_iso": "2026-06-19T22:14:03+01:00" }
```

### 6.2 `meta.json` structure & version manifest

```jsonc
{
  "schema_versions": { "meta": "0.1.0", "events": "1.0.0", "zarr_arrays": "0.1.0" },  // the one triple
  "recording_id": "2026-07-16_2300_protosom-c6",
  "subject":    { /* §6.3 */ }, "recording": { /* start/duration/site */ },
  "device":     { /* §6.3 channel registry — SOLE source of truth for channel names */ },
  "layers":     { /* §6.4 manifest */ }, "provenance": { /* §6.5 */ },
  "stats":      { /* study-level indices — the ONE home: AHI, ODI, arousal_index,
                     plmi, plmi_with_arousal, tst_s, sleep_efficiency, hrv_rmssd_overall */ }
}
```

A single semver format everywhere (`0.1.0`, not `0.1`). Readers check the major version of each axis and refuse mismatches. Study-level summary indices have exactly one home (`meta.json.stats`); `events.json` and `_meta.json` are no longer summary homes. **AHI** (the headline PSG number) and ODI/arousal index/sleep efficiency/TST are named optional fields here, computed from events + hypnogram.

### 6.3 `subject` + `device` (channel registry)

`subject` splits a de-identified core (always present, committable) from an optional `pii` block:

```json
"subject": { "subject_id": "PSG-0001", "sex": "male", "dob": "1990-01-01",
             "age_years_at_recording": 36,
             "pii": { "name": "…", "nhs_number": "9999999999", "email": "…" } }
```

`device.channels[]` is the **single canonical channel-name registry** every other slice references: each entry binds `{label (verbatim EDF), zarr_array (canonical snake_case), kind, transducer, fs_hz, storage, dtype, edf_physical_dimension, harmonized_unit, digital_min/max, physical_min/max, raw_edf_signal_index}` per §3.2/§3.3. `kind` controlled vocab: `respiratory_effort | airflow_pressure | airflow_thermal | ecg | heart_rate | rr_interval | spo2 | eeg | eog | emg | accelerometer | body_position | audio_derived | snore`.

### 6.4 `layers` — 3-layer manifest (hashing + audio anchor + per-device clock)

```jsonc
"layers": {
  "raw": {
    "biosignals": [                                  // LIST — kept a list for forward-compat,
      { "path": "raw/biometric_2026-07-16_23-00-00.edf",      // but single-device = one entry
        "format": "EDF+", "signals": 11,
        "hash": { "algorithm": "blake3", "value": "af3c…91" }, "bytes": 84672000,
        "header_start_datetime": "2026-07-16T23:00:00",       // whole-second
        "record_duration_s": 10.0,
        "device": "esp32c6",
        "device_sidecar": { "path": "raw/biometric_2026-07-16_23-00-00.json",
                            "hash": { "algorithm": "blake3", "value": "3d81…7f" } },
        "clock": { "offset_to_utc_s": 0.0, "skew_ppm": null, "source": "serial_sync+ds3231",
                   "discipline": "manual_serial", "uncertainty_s": 1.0 } }
    ],
    "audio": {
      "path": "raw/snore_16k.flac", "format": "FLAC",
      "hash": { "algorithm": "blake3", "value": "7b21…0e" }, "bytes": 921600000,
      "sample_rate_hz": 16000, "channels": 1,
      "time_anchor": { "audio_t0_offset_s": 12.734, "reference": "recording.start_datetime",
                       "method": "manual", "offset_uncertainty_s": 0.5,
                       "note": "FLAC sample 0 occurs 12.734 s AFTER t0. Positive = audio starts later." }
    }
  },
  "working": {
    "path": "working.zarr", "zarr_format": 2, "dimension_separator": ".",
    "store_hash": { "algorithm": "blake3", "value": "c4d9…aa",
                    "method": "blake3-of-sorted-(relpath\\tblake3)-list", "purpose": "integrity_only" },
    "value_digests": { "signals/thoracic": { "algorithm": "blake3",
                       "value": "…", "method": "blake3-of-decoded-LE-sample-buffer" } },  // §9 re-derivation check
    "events_sidecar": { "path": "events.json", "hash": { "algorithm": "blake3", "value": "1f0a…3c" } }
  },
  "export": {
    "clinical": { "path": "export/clinical.edf", "format": "EDF+", "deidentified": true,
                  "generated_from": "working.zarr",
                  "hash": { "algorithm": "blake3", "value": "dd54…b2" }, "bytes": 84680000 }
  }
}
```

- **Hashing: BLAKE3**, recorded as `{algorithm, value}` so a per-hash switch to `sha256` needs no schema change. ~3× SHA-256 throughput, important for the multi-GB EEG/audio envelope.
- **Zarr has no single file to hash**, so `store_hash` is a Merkle digest over the sorted `(relpath, blake3-of-file)` list. **It is integrity-only** (compressed bytes are not reproducible across lib/codec versions). Re-derivation correctness is checked against the separate **`value_digests`** — BLAKE3 over each array's decoded little-endian sample buffer, codec/chunk-independent (see §9).
- **Audio time-anchor** maps a playhead: `signal_t = audio_sample/16000 + audio_t0_offset_s`. `method` ∈ `ntp_aligned | clap_sync | manual | unknown` records *how* it was established + `offset_uncertainty_s`, so trustworthiness is auditable. A hand-entered value is `method:"manual"` with explicit uncertainty.
- **`clock`** carries `offset_to_utc_s`, `skew_ppm`, `source`, `discipline`, `uncertainty_s` — a linear model, because a constant offset is wrong by seconds over an 8 h night at ~50 ppm drift. With one device this describes **device-to-UTC** only; there is no longer a second clock to reconcile against. The ESP32-C6 is disciplined by manual serial sync against a DS3231 RTC (no NTP — the radios are off by design), so `uncertainty_s` is bounded by how carefully `tools/set_time.py` was last run, and `skew_ppm` is unmeasured. Measuring the DS3231's actual skew across a night is the cheapest available accuracy win.
- **`device_sidecar`** references the JSON the firmware itself writes next to the EDF+ (`device_uid`, timezone, sensor presence/rates, `ldc_baseline`). It is part of layer 1 and must be hashed alongside the EDF+ — it is the only record of the on-device baseline transform, and without it §6.5's `on_device_transforms` cannot be populated truthfully.

### 6.5 `provenance` — IEC-62304 reproducibility

```jsonc
"provenance": {
  "generated_at": "2026-06-20T09:00:00Z",
  "pipeline": { "repo": "sunrise-designs/ProtoSom",
                "git": { "sha": "9f2a…(full 40)", "dirty": false, "branch": "main", "describe": "v0.1.0-3-g9f2a7c1" } },
  "firmware": { "git_sha": null, "dirty": null, "equipment_string": null,
                "note": "NOT AVAILABLE — the C6 firmware sets no equipment string and compiles in no git hash. See §6.5 note." },
  "input_preparation": { "skip_samples": 700, "ignore_last_samples": 0,
                         "note": "leading 14 s dropped before DSP at 50 Hz; shifts every onset and the PLMI denominator" },
  "on_device_transforms": [ { "name": "rip_baseline_subtraction",
                              "note": "baseline captured 20 min in, averaged over 1 s, subtracted from thoracic/abdomen before EDF write; MAY be restored from RTC across a reboot",
                              "params": { "baseline_delay_s": 1200, "baseline_avg_samples": 50 },
                              "from_device_sidecar": { "ch0": 123456, "ch1": 234567, "valid": true,
                                                       "reused": false, "reset_reason": 1 } },
                            { "name": "rip_count_division",
                              "note": "raw LDC1612 counts divided per-channel before EDF write; declared physical range NOT adjusted to match, so header physical units are not nH",
                              "params": { "thoracic_divisor": 15, "abdomen_divisor": 11 } } ],
  "environment": {
    "os": "Windows 11 26200", "host_arch": "x86_64",          // re-derivation host arch matters for float determinism
    "python": { "version": "3.11.2", "lockfile": "src_python/requirements.lock",
                "lockfile_hash": { "algorithm": "blake3", "value": "5e9b…d7" } },
    "key_packages": { "numpy": "1.26.4", "scipy": "1.13.0", "zarr": "2.18.2" },
    "cpp_toolchain": { "compiler": "g++ 12.2.0", "libs": { "edflib": "1.21" } } },
  "derived_products": [
    { "product": "derived/accel0_mag", "code_ref": "signal_processing.py::accel_magnitude",
      "git_sha": "9f2a…(full)", "source_arrays": ["signals/accel0_x","accel0_y","accel0_z"],
      "params": { "window_sec": 30, "fs": 50 },
      "value_digest": { "algorithm": "blake3", "value": "…" } },
    { "product": "derived/accel_combined_mag", "code_ref": "signal_processing.py::combine_bilateral_vm",
      "git_sha": "9f2a…(full)", "source_arrays": ["derived/accel0_mag","derived/accel1_mag"],
      "params": { "threshold": 8.0, "fs": 50, "max_threshold": 500.0 },
      "value_digest": { "algorithm": "blake3", "value": "…" } },
    { "product": "events.json#plm-auto-v0", "code_ref": "signal_processing.py::count_plm",
      "git_sha": "9f2a…(full)", "source_arrays": ["derived/accel_combined_mag"],
      "params": { "threshold": 8.0, "fs": 50, "max_threshold": 500.0, "tilt_threshold_deg": 10.0,
                  "window_sec": 30 },
      "source_constants": { "lm_dur_s": [0.5,10.0], "plm_gap_s": [5,90], "plm_min_series": 4,
                            "min_gravity_mg": 100 } },
    { "product": "derived/hrv_rmssd", "code_ref": "signal_processing.py::compute_hrv",
      "git_sha": "9f2a…(full)", "source_arrays": ["signals/rr"],
      "params": { "window_sec": 300, "fs": 2.5, "rr_valid_ms": [300,2000] },
      "note": "signals/rr has NO SOURCE on this hardware — it logs zeros. Any RMSSD derived from it is meaningless and must not be emitted as a stat until R-peak detection exists.",
      "value_digest": { "algorithm": "blake3", "value": "…" } }
  ]
}
```

**Three provenance fixes the reviewers forced:**

1. **Hardcoded constants are `source_constants`, not `params`.** The AASM constants in `count_plm` (lines 45–55) are source literals, not function arguments. Presenting them as re-derivation knobs is provenance theatre — restoring them from JSON and re-running gets the source-baked values regardless. They are recorded as informational `source_constants`; the **`git_sha` + `code_ref` is their authoritative capture.** A CI assertion checks every key in `params` corresponds to a real named function argument.

2. **`window_sec` is captured at the call site, not from argparse.** `count_plm` calls `remove_baseline(data, fs=fs)` **without** `window_sec` (line 29), so PLM detection always uses the 30 s default no matter what `--window` was set — recording the argparse value would lie. (Recommend fixing the code to thread `window_sec` through; until then, capture the effective value.)

3. **`input_preparation` and `on_device_transforms` are recorded.** `--skip` (default 700 = first 70 s dropped) and the on-device RIP baseline subtraction both mutate the signal upstream of any current provenance and were captured nowhere.

`git.sha` is the **full 40-char SHA + explicit `dirty` flag** for the *pipeline*. **Firmware provenance is currently absent, not merely degraded.** The retired RPi5 firmware at least baked a short hash into the EDF equipment string; the ESP32-C6 firmware sets **no equipment string at all** and compiles in **no git hash** — there is no `edf_set_equipment` call and no `GIT_COMMIT_HASH` anywhere in `ESP32-C6-heart-idf`. Every EDF+ this hardware produces is therefore **not audit-grade** by this spec's own standard, and that is a regression against the previous device, not an improvement. The fix is the same as before: emit full SHA + dirty flag as a compile definition and write it at recording start (the 80-char equipment field is tight, so an annotation TAL is the likelier carrier). Until then, `provenance.firmware` must be explicitly `null` with a note — never silently omitted, which would read as "not applicable" rather than "missing".

---

## 7. EDF/BDF round-trip mapping & lossless guarantee

`ingest: EdfModel → ZarrModel` and `export: (ZarrModel, channel_selection, events) → EdfModel` are pure transforms; edflib (C++) / `pyedflib`/`edfio` (Python) are the only impure boundary.

EDF calibration: `physical = (digital − dmin)·(pmax − pmin)/(dmax − dmin) + pmin`.

| EDF label | rate | `storage` | Zarr array / dtype | export format | lossless? |
|---|---|---|---|---|---|
| Thoracic / Abdomen | 50 | digital | `signals/thoracic`,`abdomen` int16 | EDF+ | **bit-exact** |
| Flow | 50 | digital | `signals/flow` int16 | EDF+ | **bit-exact** |
| ECG | 100 | digital | `signals/ecg` int16 | EDF+ | **bit-exact** (phys range == digital range = identity today; guarded by test #4) |
| Accel0X/Y/Z, Accel1X/Y/Z | 50 | digital | `signals/accel{0,1}_{x,y,z}` int16 | EDF+ | **bit-exact**; phys ±2000 mg calibration preserved |
| RR | 2.5 | digital | `signals/rr` int16 | EDF+ | **bit-exact** (of zeros — the channel has no source) |
| EEG ×N (future) | 256 | digital | `signals/eeg` int32 (bit_depth 24) | **BDF+** | bit-exact, 3-byte two's-complement LE packing |

**What is lossless, precisely:** `digital(EDF_in) == digital(EDF_out)` sample-for-sample for **every** channel — and on this hardware that is now exact **by construction for all eleven**, since the firmware writes digital samples and ingest stores the integer unchanged. The old caveat about `storage="physical"` channels round-tripping only within half an EDF quantum no longer applies to any captured channel; it survives only for hypothetical future producers that write physical doubles.

**What is still lost, and it is upstream of everything here:** the RIP channels' on-device count division (`÷15`, `÷11`) is applied *before* the EDF write, so the pre-division raw LDC1612 counts are gone from the raw anchor permanently. The round-trip faithfully reproduces the divided values; it cannot recover what was divided away. Recording the divisors in provenance (§6.5) is what makes the transform *auditable*, not reversible.

**Format rule:** a file is uniformly EDF+ (2-byte) **or** BDF+ (3-byte); widths cannot mix. **Promote the whole export file to BDF+ if any selected channel has `digital_max > 32767` or `digital_min < −32768`** (i.e. 24-bit EEG). A full future montage forces all 16-bit channels to 3-byte too (~50% size inflation on those) — a stated, accepted cost; EDFBrowser reads such mixed-origin BDF correctly.

**Record-duration solver:** EDF needs `samples_per_record = rate × D` integral for every channel simultaneously. Pick the smallest `D` (LCM-of-denominators) per file. The current channel set is {50, 100, 2.5}: `D = 1 s` fails, because 2.5 Hz × 1 s = 2.5 samples — **`D` must be even-numbered in halves**, so the smallest valid `D` is **2 s** (RR → 5 samples/record). The firmware sidesteps this by using `D = 10 s` (RR → 25 samples/record), which is why `lround` is needed at `logger.cpp:494`. An exporter that naively defaults to `D = 1 s` will silently truncate the RR channel's rate to 2 Hz. A future 0.1 Hz body-position channel forces `D ≥ 10 s`. Zarr chunk length is independent of `D`.

**Events → EDF+ TAL (explicitly lossy):** each event `{onset_s, duration_s, label}` → one TAL `+<onset>\x15<duration>\x14<label>\x14\x00` (onset/duration ASCII decimal seconds from file start; the first TAL per record is the empty time-keeper `+<rec_start>\x14\x14\x00`). The rich model (type/channels/params/confidence/scoring_id) **collapses to one label string** — `events.json` is authoritative; EDF+ is a clinical-interop view. Label convention: `"OA"`, `"Hypopnea (1A)"`, `"Sleep stage N2"`, `"LM"`/`"PLM"`. **One EDF+ export per scoring** (multiple scorers cannot share one annotation stream unambiguously). onset/duration printed to 3 decimals (ms) — the documented re-parse tolerance. UTF-8 labels allowed; no `0x14`/`0x15` byte may appear in label text.

---

## 8. Language-neutrality — the C++-safe subset

**One-line contract for Dmitry:** *Zarr v2, C-order, little-endian fixed-width numeric dtypes from `{int16,int32,uint8,uint16,float32,float64}`, Blosc(zstd,shuffle) (or gzip/raw) compressor, `filters: null`, no string/object/datetime dtypes, `.zattrs` plain JSON, `dimension_separator: "."`.* Everything inside this subset round-trips identically through z5, TensorStore, xtensor-zarr, and zarr-python.

| Trap | Rule |
|---|---|
| `object`/structured dtypes (pickled) | Forbidden. Numeric fixed-width only. |
| numpy scalars/arrays in `.zattrs` | Cast to native `float`/`int`/list before writing; `.zattrs` is plain JSON. |
| string/`|S`/`|U`/VLen-UTF8 arrays | No string Zarr arrays. Categorical → `uint8` codes + `categories: [...]` attr; free text → JSON sidecar. |
| `datetime64`/`timedelta64` | No datetime dtype. Time is `int64` ns / ISO in attrs; irregular series use a `float64 *_t` companion. |
| numcodecs filters (Delta, FixedScaleOffset, PackBits, Categorize, Quantize) | `filters: null` everywhere. Byte-shuffle gives the delta benefit portably. |
| `.npy`/pickle/Pickle/MsgPack/JSON codecs | Forbidden. Chunk encoding is Blosc/zstd or gzip/raw only. |
| float fill_value | The JSON **string** `"NaN"` (and `"Infinity"`/`"-Infinity"`), never a bare token — a C++ writer (`nlohmann/json`) must emit the literal string, not `std::nan`. Integer arrays use a mask array, not in-band fill. |
| v2-vs-v3 `shuffle` encoding | v2 `.zarray` `shuffle` is the **integer** `1`/`2`; never copy a v3 string `"shuffle"` into a v2 store. |

**The DSP-domain trap (escalated, not buried):** `signals/accel0_x` holds the **int16 digital** sample in this store (−8192…8191), while the current DSP reads physical **mg** floats straight from the EDF+ via `edfio` and computes `vm` and the `threshold=8` LM cutoff in that mg space. The two differ by the header's affine map — a factor of ~0.244 mg/LSB. Adopting this schema **requires** refactoring the DSP to read `signals/accel{0,1}_{x,y,z}` from Zarr **and** either applying the calibration on read or recalibrating `threshold`, `max_threshold`, and the GPC gate's `MIN_GRAVITY_MG` into digital space — otherwise LM/PLM detection silently changes.

The safer refactor is to **apply the affine map on read** and keep the DSP entirely in mg, since every tuned constant (`threshold=8`, `max_threshold=500`, `tilt_threshold_deg=10`, `MIN_GRAVITY_MG=100`) is expressed in mg or degrees and none has been validated against a corpus (open fork **O12**). Changing the units *and* the thresholds in one step would make a detection regression indistinguishable from a units bug.

*(An earlier revision of this note described these arrays as `uint16` 12-bit ADC counts at 0–3300 mV. That was the retired ADXL335-on-RPi5 path; the MMA8451 reports signed mg. Any code written against the old description is wrong.)*

---

## 9. Property / round-trip tests (the IEC-62304 assertions)

Each is a mechanically-checkable pass/fail; the **equality criterion is defined per storage mode** so "reproduced correctly" is unambiguous.

1. **Digital exactness (headline):** for every channel, `digital(export(ingest(edf))) == digital(edf)` sample-for-sample. Exact for `storage="digital"`; holds for `storage="physical"` because calibration constants are reused. Property test over randomly generated EDF headers + buffers.
2. **Physical tolerance:** `storage="physical"` physical values equal within **±0.5 EDF quantum**. Derived float arrays: `rtol=1e-6, atol` documented per array.
3. **Header fidelity:** label, transducer, `edf_physical_dimension` (verbatim), sample rate, digital/physical min-max, start datetime, record duration survive `EDF → Zarr(attrs) → EDF`.
4. **ECG identity guard:** assert `digital_min/max = 0/4095` for `ecg` and that the phys-range==digital-range identity round-trips exactly — fails loudly if a future firmware change breaks it.
4b. **Fractional-rate guard:** assert the RR channel survives at **2.5 Hz** — that the record-duration solver never picks a `D` making `rate × D` non-integral, and that a 2.5 Hz channel round-trips without silently becoming 2 Hz. This is the `lround` bug at `logger.cpp:494` reappearing on the export side.
5. **BDF promotion:** any channel outside 16-bit ⇒ BDF+; a known ±8388607 vector round-trips exactly through 3-byte two's-complement LE packing.
6. **Annotation round-trip:** `{onset,duration,label}` → TAL bytes → re-parsed event equals original (onset/duration to ms; label byte-identical; no `0x14`/`0x15` in text).
7. **Record-duration solver:** chosen `D` makes `D×rate` integral for all channels; regression cases {50,1,5,100}, {10,1}, {0.1,1}.
8. **Cross-language conformance (CI gate):** one fixture recording written by zarr-python **and** by the chosen C++ lib produces byte-identical chunk files and `.zarray`/`.zattrs` (modulo Blosc version string) — proves the §8 subset is real. The fixture pins exact dtype/codec/shuffle-integer/`dimension_separator` so it would catch a v3-string-shuffle or a wrong fill encoding.
9. **Re-derivation value check:** re-running a derived product (e.g. `count_plm` at the recorded `git_sha` + lockfile on the hash-verified raw) reproduces events — exact integer match where the detection is integer-domain; for float-domain detection, value-equality within the §9.2 tolerance with `numpy`/`scipy`/arch pinned (float DSP is not bit-stable across BLAS/arch, which can flip `≥8` boundary cases). The `value_digests` (§6.4) make this a single comparable digest per array, independent of Zarr lib/chunking.

---

## 10. Open questions for Leon & Dmitry

1. **Vendor TensorStore, or stay on z5/xtensor-zarr?** v2 is chosen precisely to keep z5/xtensor-zarr open. If Dmitry commits to TensorStore (and explicitly drops z5), the v2-vs-v3 trade reopens and sharding becomes available — worth deciding before the high-res EEG envelope lands. *(This is the single biggest external dependency.)*
2. **Fix the RIP physical range, or bless the divisor workaround?** The firmware now recovers effective resolution by dividing raw counts (`÷15`, `÷11`) while leaving the declared ±1e6 nH range untouched — so the header lies about units. Three options: (a) tighten `physical_min/max` to match the divided scale and keep the divisors; (b) drop the divisors and tighten the range instead, which is the honest fix; (c) keep both and document the offset permanently. The divisors were tuned on **one night** with ~2× headroom and can still clip on deeper breathing, which argues against leaning on them. This remains the biggest data-quality lever in the pipeline. See open fork **O3**.
3. **PII anchor policy — largely resolved by the hardware change.** The C6 firmware writes **no** patient name or DOB into the EDF+ header (no `edf_set_patientname`/`edf_set_birthdate` calls exist), so the raw anchor is PII-free by construction and the old conflict — "the immutable hashed anchor embeds a name and cannot be scrubbed without changing its hash" — is **gone**. What remains to decide is narrower: whether `meta.json`'s `subject.pii` block ships alongside a shared working store or is stripped at share time, and whether `.gitignore` needs a `!meta.json` exception so a de-identified `meta.json` can be committed. See [privacy](../standards/privacy.md) and open fork **O9**.
4. **Refactor the DSP to read Zarr?** Confirm the EDF+→Zarr contract is go-forward (the DSP currently reads the EDF+ raw anchor directly), and `threshold=8` is recalibrated from mg-space to uint16 12-bit counts (otherwise PLM detection drifts silently).
5. ~~**One Zarr group multi-device, or one per device?**~~ **Resolved by the hardware change.** With the RPi5 retired there is one device and one clock, so every channel shares a timebase and any event may reference any channel. One group, no cross-device sync primitive needed. *(If a second device ever returns, this question returns with it — do not treat the resolution as a general principle.)*
6. **Firmware provenance upgrade — now more urgent, not less.** The C6 firmware emits **no** git hash and **no** equipment string, so it is strictly worse than the RPi5's short-hash-in-equipment. Change the build to emit full SHA + dirty flag + describe as a compile definition and have `logger.cpp` write it (the equipment field is 80-char-limited → likely a recording-start annotation TAL). Until then **every** recording from this hardware is non-audit-grade, not just those from dirty builds — is that acceptable for v0.1?
7. **Subsecond / whole-second time origin.** Firmware sets only whole-second start; accept whole-second alignment for v0.1, or have firmware pass a fractional start so the `+0.X` time-keeping TAL preserves it?
8. **Pinned environment.** `requirements.txt` is unpinned and the `requirements.lock` + `lockfile_hash` the spec references do not exist yet — generate a real lock (uv/pip-compile) and pin numpy/scipy/arch before the reproducibility block is non-fictional?
9. **Event id scope & hypnodensity.** Event ids are unique within a scoring (groups reference within-scoring ids). If the viewer needs human-confirms-algorithm cross-linking, switch to composite `(scoring_id, event_id)` keys? And if epoch granularity ever drops to per-second hypnodensity, the hypnogram becomes dense → moves to a Zarr probability matrix (deferred until on the roadmap).

---

**Key cross-slice reconciliations made (where reviewers found real breakage, the spec now reflects the fix, not the debate):** Zarr **v2** (single decision; z5/xtensor-zarr are v2-only per the [registry](https://zarr.dev/implementations/)); per-array **`storage` attr** as the one storage contract — now uniformly `digital` at capture, since `logger.cpp` writes `edfwrite_digital_samples` for all eleven channels, with `physical` surviving only in `derived/`; **mask-array** gap model (firmware writes 0, never digital_min), simplified to one mechanism now that no captured array is float; **interleaved min/max `(n,2)`** pyramid with gap-aware build and a real depth/spectrogram budget; single **canonical snake_case channel registry** in `meta.json.device.channels`; **`source_constants` vs `params`** split + `window_sec`/`--skip`/on-device-baseline capture; **`store_hash` (integrity) vs `value_digests` (re-derivation)** separated; per-storage-mode **equality tolerances** wired into the tests.

**Revised 2026-07-19 for single-device hardware:** the RPi5 is retired and the ESP32-C6 is the sole raw anchor. This *simplified* three things (one clock, one `signals/` group, one storage mode → strictly stronger bit-exactness guarantees) and *worsened* two: firmware provenance went from degraded to **absent**, and the RIP unit story got murkier with the on-device count divisors. Accel moved to 6 axes at 50 Hz in signed mg — a 10× volume increase and a units change that invalidates any code written against the old `uint16` 12-bit description.

Sources: [Zarr implementations registry](https://zarr.dev/implementations/), [TensorStore zarr3 driver](https://google.github.io/tensorstore/driver/zarr3/index.html), [xtensor-zarr](https://github.com/xtensor-stack/xtensor-zarr), [z5](https://github.com/constantinpape/z5), [EDF+ spec](https://www.edfplus.info/specs/edfplus.html), [BDF+ description](https://www.teuniz.net/edfbrowser/bdfplus%20format%20description.html), [EDFlib](https://www.teuniz.net/edflib/), [zarr-python 3.0 migration](https://zarr.readthedocs.io/en/stable/user-guide/v3_migration/), [JSON Schema 2020-12](https://json-schema.org/draft/2020-12/schema). Repo facts grounded in `ESP32-C6-heart-idf/components/logger/logger.cpp`, `src_python/signal_processing.py`, `src_python/read_log.py`, `src_python/export_zarr.py`, `.gitignore`.