---
title: Data Formats
domain: knowledge
status: living
updated: 2026-06-19
summary: The on-disk formats across the three-layer data model — EDF+/BDF+ and FLAC as the raw anchor, the Zarr v2 working store as the language-neutral boundary, and the JSON sidecars — plus the three-format history and its resolution.
---

# Data Formats

ProtoSom's pipeline (device → ingest → signal-processing → web app) carries data
through three layers, each with its own format. This page is the reference for what
those formats are, why each was chosen, and how they fit together. See
[architecture](architecture.md) for the pipeline and the three-language boundary; see the
full [zarr schema spec](../planning/zarr-schema-spec.md) for the byte-level
working-store layout.

## The three layers and their formats

| Layer | Format | Mutability | Role |
|---|---|---|---|
| **Raw anchor** | EDF+ / BDF+ (biosignals) + FLAC (audio) | immutable, content-hashed | provenance; whatever the device emits, never modified |
| **Working store** | Zarr v2 group + JSON sidecars | regenerable from the raw anchor | the Zarr boundary; what processing reads/writes and the [slicing server](architecture.md) slices |
| **Clinical export** | EDF+ / BDF+ | never stored, regenerated on demand | EDFBrowser interop |

The **Zarr store + JSON metadata is the boundary** between Dmitry's side
(**C++ ingest** + **Python processing**) and Leon's side (**the TS web app**). Three
languages meet at these files on disk: C++ ingest writes the raw Zarr layer, Python
processing reads it and writes the derived Zarr layer + sidecars, and the TS web app
**only reads** — it never writes Zarr. That is the [membrane](architecture.md).

---

## Raw anchor: EDF+ / BDF+

EDF+ (European Data Format Plus) is the raw anchor for all biosignals. It is a
self-describing format: a plain-text ASCII header declares, per channel, the label,
transducer, physical dimension (units), physical and digital min/max, and sample
rate, followed by interleaved fixed-duration data records. Each sample is stored as a
16-bit signed integer (digital), and the header's physical/digital min/max pair gives
the affine map back to physical units. C++ ingest reads/writes it via **edflib** (the
C library the devices already use; `pyEDFlib` is the equivalent Python binding).

- **EDF+ = 16-bit** (`digital_min/max = -32768/32767`). Sufficient for RIP belts,
  flow, HR/RR, and the AD8232 ECG front end.
- **BDF+ = 24-bit** is reserved for future high-resolution EEG, where 16 bits is too
  coarse. The format choice is otherwise identical; only the sample width differs.

### Current acquisition channels

The RPi5 acquisition (`src/main.cpp`) writes a **6-channel EDF+** file
(`edfopen_file_writeonly(..., EDFLIB_FILETYPE_EDFPLUS, 6)`, `src/main.cpp:126`). The
`ChannelInfo` table (`src/main.cpp:145-152`) is the ground truth:

| # | Label | Transducer | Rate (Hz) | Phys dim | Phys range |
|---|---|---|---|---|---|
| 0 | Thoracic | LDC1612 CH0 (RIP belt) | 50 | Inductance (nH) | ±1 000 000 |
| 1 | Abdomen | LDC1612 CH1 (RIP belt) | 50 | Inductance (nH) | ±1 000 000 |
| 2 | HR | Polar H9 via ESP32-S3 | 1 | BPM | 0–250 |
| 3 | RR | Polar H9 via ESP32-S3 | 5 | ms | 0–2000 |
| 4 | Flow | Sensirion SDP800-125P | 50 | Pressure | 0–1000 |
| 5 | HR_Raw | AD8232 ECG | 100 | ADC | 0–4095 |

The ESP32-S3 wrist device (`ESP32-S3-heart/.../logger.cpp`) writes its own
**4-channel EDF+**: AccelX/Y/Z (12-bit ADC, 10 Hz) + RR (ms, 1 Hz).

> **physical vs digital write path.** `main.cpp` writes **physical** doubles via
> `edfwrite_physical_samples` (`src/main.cpp:286-291`) — edflib does the
> physical→digital quantization using the declared min/max. `logger.cpp` writes
> **digital** integers via `edfwrite_digital_samples`. C++ ingest carries this
> distinction forward into the working store as a per-array `storage` attribute (see
> below) so the readers know whether a Zarr array already holds physical units or needs
> the affine map applied.

## Raw anchor: FLAC audio sidecar

Audio (for snore / breathing analysis) is captured as a **FLAC** sidecar, lossless and
time-anchored to the biosignal recording's start time. It lives beside the EDF+ file
in the raw layer and is never modified. Python processing reads this sidecar to produce
a precomputed spectrogram array in the derived Zarr layer; the viewer's spectrogram
pane renders that array (wavesurfer.js), never the raw FLAC directly.

---

## Working store: Zarr v2 — the language-neutral boundary

The working store is a **Zarr group per recording**: every dense signal becomes its
own array, chunked along the time axis, plus the JSON sidecars below. It is the
regenerable layer that all three languages share — the Zarr boundary. **C++ ingest**
writes the **raw** Zarr (via a C++ Zarr lib — TensorStore / z5 / xtensor-zarr);
**Python processing** reads raw → writes the **derived** Zarr + sidecars (via
`zarr-python`); **the TS web app** reads it (via `zarrita.js`) and never writes.

### Why Zarr v2, not v3

- **Keeps the C++ reader set open.** `z5` and `xtensor-zarr` are **v2-only**;
  TensorStore handles v2 and v3. Choosing v2 keeps all three viable for the producer.
- **Sharding (the main v3 draw) is not needed** at PoC recording sizes.
- **Reversible.** Nothing locks us to v2; we can move to v3 later *only if every chosen
  library supports it*.

This is recorded as a settled decision — see [decisions](../state/decisions.md).

### Codec and chunking — the language-neutral contract

Because three language ecosystems must read the same bytes, the encoding is pinned to
the **common subset** every library understands:

- **Codec = Blosc(zstd, shuffle).** Good ratio, fast, and — critically — readable by
  the C++ Zarr libraries and `zarrita.js` alike.
- **No Python-only `numcodecs` filters** (Delta, PackBits, etc.). A Delta-filtered
  array is silently unreadable from C++/TS, which would break the boundary. This
  exclusion is a hard rule, not a preference.
- **Chunk along time.** A window request (`/window?start&end`) then touches a bounded
  set of contiguous chunks rather than the whole array.

> The current `src_python/export_zarr.py` is a transitional helper, not the canonical
> writer. It opens the group with `zarr_format=2` (`export_zarr.py:56`) but, for the
> tiny legacy ESP32 sample, writes **one chunk per array** (`chunks=data.shape`,
> `export_zarr.py:71`) and uses Zarr defaults rather than the Blosc(zstd, shuffle)
> codec. It also predates the C++-ingest/Python-processing split — it is Python doing
> the *ingest*-shaped job of turning the legacy `.bin` into Zarr. The canonical
> chunk-along-time + Blosc store is the raw Zarr written by C++ ingest and the derived
> Zarr written by Python processing; see the
> [zarr schema spec](../planning/zarr-schema-spec.md).

### The `storage` attribute

Each array carries a per-array attribute `storage = "physical" | "digital"`,
mirroring the producer's write path (above). `physical` arrays already hold real units
(e.g. nH, BPM); `digital` arrays hold raw integers and require the EDF+ affine map
(physical/digital min/max) to interpret. Readers use this attribute to decide whether
to apply the map before charting or before a clinical export.

### Decimation pyramids (later, additive)

The viewer needs only **"the data the chart needs"** — screen-resolution for the
visible window (~a few thousand points), not the whole raw recording. Today the
slicing server decimates on the fly. Later, **precomputed multi-resolution pyramids**
(coarser copies of each array) are added as extra Zarr arrays, letting the server pick
the resolution matching the requested window — **without changing the viewer or the
windowed API**. Starting by loading the whole (tiny) recording degenerates cleanly to
"request the full range", so this is purely additive. See [architecture](architecture.md)
and the [zarr schema spec](../planning/zarr-schema-spec.md).

### Schema at a glance

One array per dense signal, chunked along time; sparse annotations and metadata live
in JSON sidecars, not in Zarr. The legacy `export_zarr.py` arrays
(`export_zarr.py:58-67`) hint at the shape — a shared `t` time axis plus per-signal
arrays (`rr`, `accel_x/y/z`, `accel_mag`, `hrv_*`) — but the canonical per-channel
schema, dtypes, chunk sizes, and attribute set are defined in the
**[full zarr schema spec](../planning/zarr-schema-spec.md)**.

---

## JSON sidecars

Two JSON files sit beside the Zarr group, written by Python processing. They hold
everything that is *not* a dense time series.

### `events.json` — sparse annotations

Sparse, irregular annotations that would waste space as dense arrays: scored events
and their spans. The legacy `_meta.json` already carries the relevant shapes —
`lm_events` (LM event `[start, end]` pairs) and `plm_groups` (grouped PLM runs)
from AASM PLM scoring (`export_zarr.py:92-93`). In the canonical store these sparse
annotations are split out into `events.json`.

### `meta.json` — metadata + provenance

Recording metadata, patient block, processing stats, and the **provenance block**. The
legacy `_meta.json` (`export_zarr.py:79-96`) shows the seed fields:

- `patient` and `recording` blocks (PII kept in a **separable block** — the clinical
  export scrubs the EDF+ header name/DOB; see [privacy](../standards/privacy.md)),
- `stats` — processing outputs (`total_lms`, `total_plms`, `plmi`, `hrv_overall`, plus
  the scoring params `threshold`, `window_sec`, `fs`),
- **provenance** — currently seeded by `git_hash` (`_git_short_hash()`,
  `export_zarr.py:13-22`) and `zarr_path`. In the canonical store the provenance block
  stamps each derived product with the processing-code version, the content hash of the
  raw anchor it was derived from, and the parameters used — so any re-score or re-export
  is auditable back to the immutable raw layer.

Full field definitions are in the [zarr schema spec](../planning/zarr-schema-spec.md).

---

## Clinical export: regenerated EDF+ / BDF+

EDF+/BDF+ is **regenerated on demand** from the working store for EDFBrowser interop —
**never stored**. It may be owned by the C++ side (reusing edflib). The export
**scrubs the EDF+ header** patient name/DOB so the file is shareable and de-identified
while the dense signals remain intact (see [privacy](../standards/privacy.md)).

---

## Format history and its resolution

Three biosignal storage formats were on the table. The resolution is settled — see
[decisions](../state/decisions.md).

1. **Legacy 5-byte `.bin`** — the original ESP32 wrist-device log: 5 bytes per record
   (3 × `uint8` accel X/Y/Z + 1 × `uint16` RR), sampled at 10 Hz. The committed
   `biometric_filtered.bin` is the one surviving example. It is **anonymous sample
   biometric data** (accel + RR, no identifiers), kept only so the legacy pipeline has
   something to run against; `.gitignore` carries an explicit `!biometric_filtered.bin`
   exception (see [privacy](../standards/privacy.md)). **Status: legacy, being retired.**
   Current hardware writes EDF+.

2. **Proposed proprietary binary** — a custom on-disk format was considered for the raw
   anchor. **Status: dropped.** It would have meant maintaining our own reader/writer
   across the boundary and forfeiting tool interop, for no gain over EDF+.

3. **EDF+ (+ BDF+ for future EEG)** — **the chosen raw anchor.** Self-describing,
   16/24-bit, readable by edflib and by every clinical tool (EDFBrowser), and already
   what `main.cpp` and `logger.cpp` write. Audio is the FLAC sidecar.

**Resolution:** EDF+/BDF+ is the raw anchor for biosignals; FLAC is the audio sidecar;
the proprietary binary is dropped; the legacy `.bin` is retired down to the single
sample. Everything downstream of the raw anchor lives in the Zarr working store and the
JSON sidecars described above — the language-neutral boundary between C++ ingest +
Python processing and the TS web app.

---

## See also

- [architecture](architecture.md) — pipeline, three-language boundary, the membrane.
- [zarr schema spec](../planning/zarr-schema-spec.md) — byte-level working-store layout.
- [decisions](../state/decisions.md) — settled choices and open forks.
- [privacy](../standards/privacy.md) — PII handling and de-identified export.
