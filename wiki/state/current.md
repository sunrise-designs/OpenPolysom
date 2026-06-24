---
title: Current State
domain: state
status: living
updated: 2026-06-19
summary: What exists in the repo today (C++ EDF+ writers, legacy Python DSP on the 5-byte .bin, a prototype ECharts/Zarr TS viewer) versus the planned three-layer pipeline — and where the gaps are.
---

# Current State

A blunt snapshot of what is **actually committed and runnable today** against the
[settled architecture](../knowledge/architecture.md). This is a proof-of-concept (PoC):
much of the four-stage pipeline (device → ingest → signal-processing → web app) and the
three-layer data model (raw anchor → working store → clinical export) is **planned, not
built**. The honest headline: there is **no EDF→raw-Zarr ingest and no Zarr→EDF clinical
export yet** — the language boundary is real in intent but only partly wired in code.

See also: [data formats](../knowledge/data-formats.md) ·
[signal processing](../knowledge/signal-processing.md) · [viewer](../knowledge/viewer.md) ·
[hardware](../knowledge/hardware.md) · [decisions / open forks](decisions.md) ·
[pipeline assessment](../planning/pipeline-assessment.md).

---

## What exists today

### C++ ingest side — EDF+ writers only (no Zarr yet)
- **RPi5 acquisition** (`src/main.cpp`): the live device path. Writes a 6-channel EDF+
  via edflib — Thoracic + Abdomen (LDC1612 RIP belts, nH, 50 Hz), HR (BPM, 1 Hz),
  RR (ms, 5 Hz), Flow (SDP800, 50 Hz), HR_Raw (AD8232 ECG, 100 Hz). Physical samples
  (`edfwrite_physical_samples`).
- **ESP32-S3 wrist device** (`ESP32-S3-heart/BLE_HR_plus_accel_ADC/logger.cpp`): 4-channel
  EDF+ — AccelX/Y/Z (12-bit @10 Hz) + RR (@1 Hz). Digital samples
  (`edfwrite_digital_samples`).
- Build: `CMakeLists.txt` at repo root; `./setup_env.sh` + `./run_protosom.sh` on the Pi
  (README "How to build / How to run").

These are **the only parts of the C++ ingest that exist**. The C++ stage that converts
raw EDF+/FLAC into the **raw Zarr** layer — the actual [Zarr boundary](../knowledge/data-formats.md)
write — **does not exist**. Neither does the C++ **clinical export** (Zarr→EDF+/BDF+).

### Python processing side — legacy DSP on the 5-byte .bin
The current Python (`src_python/`) does **not** read the EDF+ the C++ writers produce.
It reads the **legacy 5-byte `.bin`** format (3×uint8 accel + uint16 RR @10 Hz) — a
format the architecture marks for **retirement**. Entry point `read_log.py`:
- Parses records with `struct.unpack_from('<H', ...)` over 5-byte strides
  (`read_log.py:90`, `:119`).
- `signal_processing.py`: `remove_baseline` (median-window), `count_plm` (AASM PLM/LM
  scoring), `compute_hrv` (RMSSD), `accel_magnitude`. These match the
  [signal-processing](../knowledge/signal-processing.md) intent but run on accel + RR
  only — none of the RIP / flow / airflow / snore work exists yet.
- The only committed input is `biometric_filtered.bin` (anonymous accel + RR sample;
  the explicit `!biometric_filtered.bin` exception in `.gitignore`).
- Typical invocation ("how to use.md"):
  `python read_log.py -f biometric_filtered.bin -c --threshold 3 --skip 1500 --ignore_last 2500`.

### The Zarr write that does exist (Python, partial, not to spec)
`export_zarr.py:save_zarr_json` is the **only Zarr writer in the repo**, and it writes the
**derived layer only** (no raw layer feeds it):
- Zarr **v2** group, one array per series: `t`, `rr`, `accel_x/y/z` (uint8),
  `accel_mag`, `hrv_t`, `hrv_rmssd` (`export_zarr.py:58-67`).
- **Single chunk per array** and the **default codec** — i.e. **no Blosc(zstd, shuffle)
  yet**, no time-chunking, no `storage=physical|digital` attrs. The v2 choice is correct
  per [decisions](decisions.md); the codec + chunking are **not yet** to the
  [data-formats](../knowledge/data-formats.md) spec.
- JSON sidecar `<stem>_meta.json` carries patient/recording/stats/events plus a
  `git_hash` provenance stamp (`export_zarr.py:79-97`) — close to the planned `meta.json`,
  but events live in this single sidecar, **not** a separate `events.json`.

### TS web app — prototype viewer on the unmaintained zarr.js
`src_web/` reads the Zarr + sidecar and draws charts — the presentation role is real,
but the scaffolding is PoC-thin:
- `src/main.ts` → `loadMeta` + `loadZarr` (`zarr_loader.ts`) → ECharts (`chart.ts`),
  dark canvas renderer.
- **Reads Zarr via `zarr` (zarr.js)** — `HTTPStore` + `openArray` (`zarr_loader.ts:1`).
  This is the **unmaintained** library; the architecture calls for **zarrita.js**.
- **No `package.json`, no `tsconfig.json`, no lockfile.** The build is a single bare
  `esbuild` call in `src_web/build.sh`. (Note: the architecture brief's "empty
  package.json/tsconfig" is generous — they are simply **absent**.)
- A **prebuilt bundle is committed**: `src_web/dist/chart.js`. The viewer is **browser-direct**
  at PoC scale — it loads the whole (tiny) recording.

### Serving — a Python http.server, not a TS slicing server
There is **no TS slicing server**. `export_zarr.py:serve_and_open` spins up a Python
`http.server` (`SimpleHTTPRequestHandler`) on an ephemeral port, stitches `index.html` +
`dist/chart.js` from `src_web/` together with the recording dir, and opens the browser at
`/index.html?meta=<sidecar>` (`export_zarr.py:103-140`). So today **Python both writes the
Zarr and serves the viewer** — the language boundary is blurred in the running system.

---

## Existing vs planned (one table)

| Concern | Today (committed) | Planned (settled) |
|---|---|---|
| **Raw anchor** | EDF+ written by C++; legacy 5-byte `.bin` still the Python input | EDF+/FLAC, content-hashed, immutable |
| **C++ ingest → raw Zarr** | **Does not exist** | C++ (TensorStore/z5) writes raw Zarr + header metadata |
| **Python processing input** | Legacy 5-byte `.bin` | Raw Zarr (zarr-python) |
| **Derived layer** | `export_zarr.py` writes v2, 1 chunk/array, default codec, single sidecar | Per-array time-chunked Zarr v2 + Blosc(zstd,shuffle), `events.json` + `meta.json` |
| **TS reader** | zarr.js (unmaintained), no manifest | zarrita.js, proper TS project |
| **Serving** | Python `http.server` | Browser-direct now; thin **TS slicing server** as data grows |
| **Clinical export (Zarr→EDF+/BDF+)** | **Does not exist** | Regenerated on demand (likely C++) |
| **Signals processed** | Accel + RR only (PLM, HRV) | + RIP baseline (QDC+airPLS), airflow, apnea/hypopnea, snore VOTE/MFCC |

---

## Honest gaps (the "not built yet" list)
1. **No EDF→raw-Zarr ingest.** The C++ writers stop at EDF+; nothing converts EDF+/FLAC
   into the raw Zarr layer.
2. **No Zarr→EDF+/BDF+ clinical export.** The third data layer is design-only.
3. **Python reads the legacy `.bin`, not EDF+.** The retired format is still the de-facto
   processing input; the EDF+ → Python path is unwired.
4. **Derived Zarr is off-spec.** v2 is right, but no Blosc codec, no time-chunking, no
   `storage=` attrs, and events are in the meta sidecar rather than `events.json`.
5. **Viewer uses zarr.js, not zarrita.js**, and has no `package.json` / `tsconfig.json`.
6. **No TS slicing server** and **no windowed API** (`/window`, `/spectrogram`, `/meta`,
   `/events`) — Python `http.server` serves the whole recording.
7. **Most signals unprocessed.** RIP belts, nasal flow, and snore audio are captured (or
   captured-by-design) but only accel + RR are analysed.

---

## PII status (as committed)
`.gitignore` excludes `patient.cfg`, `*.json`, `*.edf`, `*.csv`, `*.bin` (with the explicit
`!biometric_filtered.bin` exception). `biometric_filtered.bin` is **anonymous** sample data
(accel + RR, no identifiers). No real PII is or was committed. The planned clinical export
will scrub the EDF+ header name/DOB; see [data formats](../knowledge/data-formats.md).
