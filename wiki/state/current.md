---
title: Current State
domain: state
status: living
updated: 2026-07-17
summary: What exists in the repo today (C++ EDF+ writers, Python DSP reading EDF+ directly via edfio, a prototype ECharts/Zarr TS viewer that now carries both accelerometers plus a combined bilateral score and a brush-selected windowed-metrics card, a standalone FastAPI windowed-metrics service, and a multi-study Netlify landing page) versus the planned three-layer pipeline — and where the gaps are.
---

# Current State

A blunt snapshot of what is **actually committed and runnable today** against the
[settled architecture](../knowledge/architecture.md). This is a proof-of-concept (PoC):
much of the four-stage pipeline (device → ingest → signal-processing → web app) and the
three-layer data model (raw anchor → working store → clinical export) is **planned, not
built**. The honest headline: there is **no EDF→raw-Zarr ingest and no Zarr→EDF clinical
export yet** — the language boundary is real in intent but only partly wired in code.
The Python side reads EDF+ **directly** (via `edfio`), which is still a shortcut
relative to the target (Python is meant to read the **raw Zarr**, not the raw
anchor EDF+ itself).

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
- **ESP32-C6 wrist device** (`ESP32-C6-heart-idf/components/logger/logger.cpp`): 11-channel
  EDF+ — Thoracic, Abdomen, Flow, ECG, Accel0X/Y/Z, Accel1X/Y/Z, RR. Runs offline (no Wi-Fi/BLE),
  features an SH1106 I2C display and serial time sync.
- Build: `CMakeLists.txt` at repo root; `./setup_env.sh` + `./run_protosom.sh` on the Pi
  (README "How to build / How to run").

These are **the only parts of the C++ ingest that exist**. The C++ stage that converts
raw EDF+/FLAC into the **raw Zarr** layer — the actual [Zarr boundary](../knowledge/data-formats.md)
write — **does not exist**. Neither does the C++ **clinical export** (Zarr→EDF+/BDF+).

### Python processing side — reads EDF+ directly
The current Python (`src_python/`) reads the **EDF+ raw anchor directly** — not
via a raw Zarr layer, since that C++ ingest stage still doesn't exist. Entry
point `read_log.py`:
- `edf_reader.py` loads the ESP32-C6 wrist-logger EDF+ via `edfio`, keyed
  off the 11-signal layout in `ESP32-C6-heart-idf/components/logger/logger.cpp`
  (Thoracic, Abdomen, Flow, ECG, Accel0X/Y/Z, Accel1X/Y/Z, RR), returning
  physical-unit arrays + each signal's native sample rate.
- `signal_processing.py`: `remove_baseline` (median-window), `count_plm` (AASM PLM/LM
  scoring), `compute_hrv` (RMSSD), `accel_magnitude`, `combine_bilateral_vm`, and the
  Gross-Position-Change gate (`gravity_baseline`/`gravity_tilt`/`gpc_mask` + `count_plm`'s
  `max_threshold`/`tilt_threshold_deg` — see [decisions § S11](decisions.md)). These
  take plain physical-unit arrays + `fs` directly. `count_plm`/`accel_magnitude` run
  once per accelerometer (Accel0, Accel1); `combine_bilateral_vm` then re-scores the
  elementwise-max envelope of both legs' vector magnitudes as the headline bilateral
  score — all three (Accel0, Accel1, combined) are now exported and plotted, not just
  computed and printed (see [signal processing](../knowledge/signal-processing.md) §3
  and [viewer](../knowledge/viewer.md)). `compute_hrv` runs against the native 1 Hz RR
  channel. RIP baseline (airPLS/QDC), airflow, and snore work still don't exist.
- Typical invocation ("how to use.md"):
  `python read_log.py -f "biometric_2026-07-08_01-57-52.edf" -c --threshold 3 --skip 150 --ignore_last 250`.

### The Zarr write that does exist (Python, partial, not to spec)
`export_zarr.py:save_zarr_json` is the **only Zarr writer in the repo**, and it writes the
**derived layer only** (no raw layer feeds it):
- Zarr **v2** group, one array per series: `t`, `rr`, `accel_x/y/z` (float32,
  physical mg — Accel0's raw axes only; Accel1's raw axes are still not carried
  through, only its scored vector magnitude — see [viewer](../knowledge/viewer.md)),
  `accel_mag` (Accel0's vector magnitude), `hrv_t`, `hrv_rmssd`
  (`export_zarr.py:58-67`), plus `accel1_mag` and `accel_combined_mag` (Accel1's own
  and the bilateral-combined vector magnitude) whenever `read_log.py` scored a second
  accelerometer.
- **Single chunk per array** and the **default codec** — i.e. **no Blosc(zstd, shuffle)
  yet**, no time-chunking, no `storage=physical|digital` attrs. The v2 choice is correct
  per [decisions](decisions.md); the codec + chunking are **not yet** to the
  [data-formats](../knowledge/data-formats.md) spec.
- `meta.json` and `events.json` follow the canonical nested schema — `schema_versions`,
  `subject` (de-identified `subject_id` + a separable `pii` block), `recording`, `stats`
  (incl. `hrv_rmssd_overall`, and — when two accelerometers were scored — a
  `stats.legs.accel0`/`.accel1` per-leg breakdown alongside the combined headline
  numbers), `layers.raw`/`layers.working`, and `provenance` (full git SHA + dirty flag +
  branch, raw-anchor content hash, input trim). `events.json` is a real sidecar carrying
  the scored `limb_movement` events and `plm_series` groups — one `scorings[]` entry per
  channel (combined, Accel0, Accel1) when both accelerometers were scored, each
  event/group tagged `channels: [<zarr array name>]` so the viewer can tell which pane a
  span belongs to.

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

### Hosting — a multi-study Netlify site, incrementally deployed
`src_python/deploy.py` deploys to a shared Netlify site rather than one recording per
site: each run adds the study under `studies/<recording_id>/` alongside whatever was
deployed before, updates a site-root `studies.json` manifest, and re-deploys. The site
root is now a **landing page** (`src_web/src/landing.ts`) listing every deployed study
and linking to `index.html?meta=studies/<recording_id>/meta.json`. Deploys use
Netlify's SHA1-digest manifest API (folding in the previous deploy's file list) so
only new/changed bytes are actually uploaded — the shared viewer shell and any
already-deployed study's data are not re-shipped on every deploy. See
[viewer § multi-study hosting](../knowledge/viewer.md) for the mechanics.

### Windowed clinical metrics — a new standalone FastAPI service (built)
[`src_python/metrics_service.py`](../../src_python/metrics_service.py) is a real, separate FastAPI
process (`serve_metrics.py`, own port) computing a metric (currently windowed PLMI, via
[`metrics_registry.py`](../../src_python/metrics_registry.py)) over a brush-selected chart window.
The viewer POSTs to it from `main.ts`'s `wireBrushSync` and renders the result into a right-rail
card. This is distinct from the deferred raw-sample slicing server (O10) below — see
[decisions § S10](decisions.md) and [viewer](../knowledge/viewer.md). It requires `accel1_x/y/z`
raw arrays in the derived Zarr (now written by `export_zarr.py` when a second accelerometer is
scored) to recompute PLM for Accel1/the bilateral-combined channel over an arbitrary window.

### Serving — a Python dev server, not a TS slicing server
There is **no TS slicing server**. `export_zarr.py:serve_and_open` spins up a
`ThreadingHTTPServer` on an ephemeral port, stitches `index.html` + `dist/chart.js` +
static assets (`styles.css`, `sw.js`) from `src_web/` together with the recording dir,
and opens the browser at `/index.html?meta=<sidecar>`. Threaded because the per-channel
viewer opens many concurrent requests (one per Zarr array) that a single-threaded
`HTTPServer`'s small connection backlog can't keep up with. So today **Python both
writes the Zarr and serves the viewer** — the language boundary is blurred in the
running system.

---

## Existing vs planned (one table)

| Concern | Today (committed) | Planned (settled) |
|---|---|---|
| **Raw anchor** | EDF+ written by C++ (RPi5 + ESP32-C6) | EDF+/FLAC, content-hashed, immutable |
| **C++ ingest → raw Zarr** | **Does not exist** | C++ (TensorStore/z5) writes raw Zarr + header metadata |
| **Python processing input** | EDF+ directly, via `edf_reader.py` (`edfio`) — not yet the raw Zarr | Raw Zarr (zarr-python) |
| **Derived layer** | `export_zarr.py` writes v2, 1 chunk/array, default codec; `meta.json` + `events.json` are schema-compliant | Per-array time-chunked Zarr v2 + Blosc(zstd,shuffle) |
| **TS reader** | zarr.js (unmaintained), no manifest | zarrita.js, proper TS project |
| **Serving** | Python `ThreadingHTTPServer` (dev-only) | Browser-direct now; thin **TS slicing server** as data grows |
| **Clinical export (Zarr→EDF+/BDF+)** | **Does not exist** | Regenerated on demand (likely C++) |
| **Signals processed** | Accel (both accelerometers, PLM/LM + bilateral combined score) + RR (HRV) | + RIP baseline (QDC+airPLS), airflow, apnea/hypopnea, snore VOTE/MFCC |

---

## Honest gaps (the "not built yet" list)
1. **No EDF→raw-Zarr ingest.** The C++ writers stop at EDF+; nothing converts EDF+/FLAC
   into the raw Zarr layer.
2. **No Zarr→EDF+/BDF+ clinical export.** The third data layer is design-only.
3. **Python reads EDF+ directly, not via the raw Zarr.** It bypasses the raw Zarr
   layer entirely and reads the EDF+ raw anchor itself, since C++ ingest → raw Zarr
   doesn't exist yet.
4. **Derived Zarr arrays are off-spec.** v2 is right, but no Blosc codec, no
   time-chunking, no `storage=` attrs — `meta.json`/`events.json` content itself is
   already schema-compliant.
5. **Viewer uses zarr.js, not zarrita.js**, and has no `package.json` / `tsconfig.json`.
6. **No TS slicing server** and **no windowed API** (`/window`, `/spectrogram`, `/meta`,
   `/events`) — a Python `ThreadingHTTPServer` serves the whole recording.
7. **Most signals unprocessed.** RIP belts, nasal flow, and snore audio are captured (or
   captured-by-design) but only accel + RR are analysed.

---

## PII status (as committed)
`.gitignore` excludes `patient.cfg`, `*.json`, `*.edf`, `*.csv`. No real PII is or was
committed. The planned clinical export will scrub the EDF+ header name/DOB; see
[data formats](../knowledge/data-formats.md).
