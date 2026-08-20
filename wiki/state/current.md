---
title: Current State
domain: state
status: living
updated: 2026-08-20
summary: What exists in the repo today (C++ EDF+ writers plus an opt-in Wi-Fi live-sample streamer, Python DSP reading EDF+ directly via edfio, an ECharts/zarrita TS viewer with both a batch and a real-time mode, a standalone FastAPI windowed-metrics service, and a multi-study Netlify landing page) versus the planned three-layer pipeline — and where the gaps are.
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
- **ESP32-C6** (`ESP32-C6-heart-idf/components/logger/logger.cpp`): the **only** device path.
  Writes an 11-channel EDF+ via edflib in 10 s data records — Thoracic + Abdomen (LDC1612 RIP
  belts, 50 Hz), Flow (SDP800, mbar, 50 Hz), ECG (AD8232 ADC, 100 Hz), Accel0X/Y/Z + Accel1X/Y/Z
  (MMA8451 ×2, mg, 50 Hz), RR (ms, 2.5 Hz — **dead channel, logs zeros**). Digital samples
  (`edfwrite_digital_samples`). Runs offline (no Wi-Fi/BLE), features an SH1106 I2C display,
  a DS3231 RTC, and serial time sync. Its RIP baseline survives a mid-study reboot via RTC memory
  (restored on any non-power-on `esp_reset_reason()`), and the JSON sidecar records the baseline
  actually used in an `ldc_baseline` block — see [hardware § RIP baseline](../knowledge/hardware.md).
- **`components/rt_stream`** (new, 2026-08-09) serves those same samples over a Wi-Fi
  WebSocket for the viewer's RT mode — opt-in per device, radio off by default, and
  reusing `logger.cpp`'s own digital conversions so a streamed point is the recorded
  integer. See [hardware § live streaming](../knowledge/hardware.md) and
  [decisions § S12](decisions.md).
- Build: ESP-IDF v6.0.1 (`idf.py build`, target `esp32c6`); see
  `ESP32-C6-heart-idf/.claude/CLAUDE.md` for the explicit-environment invocation this machine needs.
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

### TS web app — two modes, batch and RT
`src_web/` reads the Zarr + sidecar and draws charts, and since 2026-08-09 also plots
a live stream from the device:
- `src/main.ts` routes three ways: `?rt=` → live mode, `?meta=` → a stored recording
  (`loadMeta` + `loadZarr` → ECharts), neither → the landing page.
- **Reads Zarr via `zarrita`** (`FetchStore` + `zarr.open`/`zarr.get`, `zarr_loader.ts`) —
  the migration off the unmaintained `zarr.js` has landed. (`zarr` is still in
  `package.json`'s dependencies but is no longer imported.)
- **Real project scaffolding exists**: `package.json` (build / lint / typecheck / test),
  `tsconfig.json` (`strict`), `eslint.config.js` (strictTypeChecked +
  `eslint-plugin-functional`), `vitest.config.ts`, and a lockfile.
- **RT mode** (`src/rt_*.ts`, `src/signals.ts`) plots the eleven raw EDF+ channels live
  from the device's WebSocket, keeping everything since connect in growable interleaved
  typed arrays. Everything downstream of Python processing is absent rather than faked.
  `tools/mock_rt_server.mjs` replays a recorded `.edf` over the same protocol so the
  mode is runnable without hardware. See [viewer § RT vs batch](../knowledge/viewer.md).
- The bundle `src_web/dist/chart.js` is **built locally, not committed** — `.gitignore:18`
  excludes `src_web/dist/`. (Earlier notes here and in [packaging](../planning/packaging.md)
  said it was committed; it is not, so `npm run build` is a prerequisite for
  `deploy.py` and for `export_zarr.py`'s dev server, both of which read it off disk.)
  The viewer is **browser-direct** at PoC scale — it loads the whole (tiny) recording.

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
| **Raw anchor** | EDF+ written by C++ (ESP32-C6, 11-channel) | EDF+/FLAC, content-hashed, immutable |
| **C++ ingest → raw Zarr** | **Does not exist** | C++ (TensorStore/z5) writes raw Zarr + header metadata |
| **Python processing input** | EDF+ directly, via `edf_reader.py` (`edfio`) — not yet the raw Zarr | Raw Zarr (zarr-python) |
| **Derived layer** | `export_zarr.py` writes v2, 1 chunk/array, default codec; `meta.json` + `events.json` are schema-compliant | Per-array time-chunked Zarr v2 + Blosc(zstd,shuffle) |
| **TS reader** | zarrita.js, `package.json`/`tsconfig`/ESLint/Vitest in place | zarrita.js, proper TS project — **done** |
| **Live view** | RT mode: device Wi-Fi WebSocket → 11 raw EDF+ channels, browser-direct | n/a — additive to the batch path, see [decisions § S12](decisions.md) |
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
5. ~~Viewer uses zarr.js~~ — **closed.** It reads via zarrita and has a real TS
   project (package/tsconfig/ESLint/Vitest). The unused `zarr` dependency is still
   listed in `package.json` and could be dropped.
6. **No TS slicing server** and **no windowed API** (`/window`, `/spectrogram`, `/meta`,
   `/events`) — a Python `ThreadingHTTPServer` serves the whole recording.
7. **Most signals unprocessed.** RIP belts, nasal flow, and snore audio are captured (or
   captured-by-design) but only accel + RR are analysed.

---

## PII status (as committed)
`.gitignore` excludes `patient.cfg`, `*.json`, `*.edf`, `*.csv`. No real PII is or was
committed. The planned clinical export will scrub the EDF+ header name/DOB; see
[data formats](../knowledge/data-formats.md).
