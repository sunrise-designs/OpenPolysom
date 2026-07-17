---
title: Decision Log
domain: state
status: living
updated: 2026-07-17
summary: The settled architecture decisions for ProtoSom Component 2 and the open forks still owed to Leon and Dmitry.
---

# Decision Log

This is the single source of truth for what is **settled** versus **open** in the ProtoSom data
pipeline (Component 2: device → ingest → signal-processing → web app). Settled decisions were agreed
by Leon and Dmitry; the wiki ([architecture](../knowledge/architecture.md),
[data formats](../knowledge/data-formats.md), [signal processing](../knowledge/signal-processing.md),
[viewer](../knowledge/viewer.md)) is written to reflect them. Open forks are genuinely undecided —
do not invent a resolution; link here.

ProtoSom is a proof-of-concept and aspires *voluntarily* to IEC 62304 discipline (traceability, test
coverage, reproducibility). It is **not** a regulated medical device.

---

## A. Settled decisions

Each is considered closed for the PoC. Reopen only by editing this page with a dated rationale.

| # | Decision | One-line rationale |
|---|----------|--------------------|
| S1 | **Three-layer data model**: (1) raw anchor, (2) working store, (3) clinical export. | The raw anchor is the immutable provenance record; the working store is regenerable; the clinical export is on-demand and never stored. See [data formats](../knowledge/data-formats.md). |
| S2 | **EDF+ is the raw anchor for biosignals; FLAC is the time-anchored raw audio sidecar.** | EDF+ is the de-facto clinical PSG format (read via edflib, already written by the device); FLAC is lossless and time-stampable. The raw anchor is content-hashed and never modified. |
| S3 | **Zarr is the working store and the language-neutral boundary** — one array per dense signal (chunked along time) + `events.json` (sparse) + `meta.json` (metadata + provenance). | Zarr is read/written across all three languages, so it is the contract where the three sides meet. |
| S4 | **Zarr v2 + Blosc(zstd, shuffle) codec; NO Python-only numcodecs filters** (no Delta, no PackBits). | The store must be readable by C++ (TensorStore/z5) and TS (zarrita.js), not just Python — so only language-neutral codecs are allowed. z5 is v2-only, hence v2 default (see open fork O5). |
| S5 | **Language boundary: C++ ingests, Python processes, TypeScript presents.** | Each language is chosen for its strength: C++ for acquisition/performance/compliance, Python for DSP+ML, TS for the viewer. They meet at the Zarr store + metadata. See [architecture](../knowledge/architecture.md). |
| S6 | **The TS web app READS Zarr and never writes it.** | Keeps the boundary one-directional: only C++ ingest writes the raw Zarr layer and only Python processing writes the derived layer; the viewer is a pure reader. |
| S7 | **Drop the proposed "proprietary binary" capture format.** | EDF+ (biosignals) + FLAC (audio) cover the raw anchor with standard, tooling-supported formats; a bespoke format adds no value and harms auditability. |
| S8 | **Clinical EDF+/BDF+ export, regenerated on demand, never stored.** | EDFBrowser interop without a third persisted copy; the export scrubs the EDF+ header (patient name/DOB) for a de-identified shareable file. May be owned by C++ ingest. |
| S9 | **Cross-platform: the TS web app is the single core — installable PWA now, webview-wrap later (Capacitor = mobile, Tauri 2 = desktop/air-gapped). NOT React Native or non-JS (Flutter/KMP/.NET MAUI).** (decided 2026-06-20) | A stack survey confirmed RN's Hermes engine has **no production WebAssembly** ([Polygen is build-time `wasm2c` only](https://github.com/callstackincubator/polygen)) — breaking zarrita's Blosc decode — and RN-native can't render ECharts (canvas), so RN/non-JS would force rewriting the charts **and** the Zarr boundary (the two crown jewels). Webview wrappers reuse ~100% of the TS + ECharts + zarrita stack; [Tauri 2](https://v2.tauri.app/)'s Rust shell can also spawn the C++ ingest / Python processing per the on-demand model. Shipped now as an installable, offline-capable PWA. |
| S10 | **Windowed clinical-metrics service: a new, separate on-demand compute API — FastAPI (Python) now, Rust-compatible HTTP contract for later — for computing a derived metric (e.g. windowed PLMI) over a user brush-selected chart window.** (decided 2026-07-09) | Distinct from **O10**: O10 is the deferred *raw-sample* slicing server (`/window?start&end&channels&res`, decimated samples for chart rendering, explicitly TS-only). This is a different concern — an on-demand *computed clinical number*, not raw samples — and the user (owner of Python processing + C++ ingest) directed it Python-now/Rust-later. Runs as its own process/port (`src_python/metrics_service.py` + `serve_metrics.py`), separate from the static viewer server, so the static site (Netlify) and the compute service can be deployed/scaled independently and the compute layer can be swapped for Rust without touching the TS side. See `src_python/metrics_registry.py` (the per-metric extensibility seam) and `src_python/metrics_windowing.py` (the boundary-padding strategy AASM-style windowed metrics need). |
| S11 | **Gross Position Changes are excluded from LM/PLM scoring, by two independent gates: a rotation-vs-gravity gate (`tilt_threshold_deg`, default 10°) and an amplitude ceiling (`max_threshold`, default 500 mg).** (decided 2026-07-17) | Raised by Dmitry against `biometric_2026-07-16_23-00-00.zarr`: position changes swing far larger amplitudes than limb movements and, once the baseline is removed, **draft** spurious LMs either side of themselves — one roll manufactured seven "LMs". AASM's existing `MAX_DUR` ≤10 s rule catches only the *slow* position changes. The load-bearing discriminator is **rotation**: a jerk leaves the sensor's orientation unchanged, a position change does not, and amplitude alone cannot tell them apart. The ceiling is the secondary gate, for a violent transient that never rotated the sensor (a knock, the unit coming off). Defaults are **tuned against that one recording** and are scale-dependent placeholders alongside `threshold=8` — the tilt distribution there is bimodal enough (p90 0.09° vs 13–124° at the four GPCs) that any threshold in 5–15° behaves identically, so the 10° default is not a knife-edge; the 500 mg ceiling is the weaker claim and the likelier source of a false rejection. Recalibration against a corpus is owed — see open fork **O12**. Both gates are recorded per-run in `meta.json`'s `stats`, since `total_lms` cannot be interpreted without knowing what was excluded from it. See [signal processing § 2b](../knowledge/signal-processing.md). |

Canonical terms used throughout the wiki: **raw anchor**, **working store**, **derived layer**,
**clinical export**, **C++ ingest**, **Python processing**, **the TS web app**, **the slicing
server**, **the Zarr boundary**.

---

## B. Open forks

Undecided, owed to Leon and/or Dmitry. Each has the question, the options, and who owns the call.
Nothing here should be treated as decided by downstream wiki pages.

| # | Fork | Options | Owner |
|---|------|---------|-------|
| O1 | **PDF report language** | Python (matplotlib + pikepdf, reuses the [Python processing](../knowledge/signal-processing.md) runtime; `rcParams['pdf.fonttype']=42` for selectable text + a real bookmark/outline tree) **vs** TS (pdf-lib, reuses the web-app chart rendering). | Leon + Dmitry |
| O2 | **Long-term Python vs C++ for medical-grade processing** | Keep Python (prototyping standard: scipy/numpy/scikit/PyTorch) **vs** migrate processing to C++23 later (performance + compliance). Tracked as a *risk*, not a now-change. | Dmitry |
| O3 | **RIP physical-range fix (device)** | Confirm/correct the physical range and units for the LDC1612 Thoracic + Abdomen RIP belts written by `src/main.cpp`. Affects EDF+ header scaling at the raw anchor. | Dmitry (device) |
| O4 | **24-bit EEG: EDF+ vs BDF+** | EDF+ (16-bit) is the current raw anchor; EEG at 24-bit resolution likely needs BDF+. Decide before EEG is wired in. | Dmitry (device) |
| O5 | **Zarr v2 vs v3** | Default v2 (z5 is v2-only). Move to v3 only if *every* chosen lib (TensorStore, z5/xtensor-zarr, zarr-python, zarrita.js) supports it. Constrains S4. | Leon + Dmitry |
| O6 | **AASM event-taxonomy scope** | Which events to score/store in `events.json` for the PoC (PLM/LM exist; apnea/hypopnea, arousals, snore VOTE, etc. pending). See [signal processing](../knowledge/signal-processing.md). | Dmitry |
| O7 | **Charting library** | ECharts (canvas + LTTB, works today) **vs** uPlot (MIT, ~50KB, lighter). See [viewer](../knowledge/viewer.md). | Leon |
| O8 | **ML approach + timeline** | Where ML enters (snore VOTE/MFCC classification, apnea/hypopnea detection, airPLS baseline) and when — model choice, training data, validation. | Dmitry |
| O9 | **PII de-identification policy** | Exact rules for what the [clinical export](../knowledge/data-formats.md) scrubs and how PII is kept in a separable block. `.gitignore` already excludes PII; this is the operational policy, not the repo hygiene. | Leon + Dmitry |
| O10 | **Slicing-server timing** — **DECIDED 2026-06-21: defer.** Stay **browser-direct** (only ever one night on screen at a time). The lever for scale is **decimation pyramids** (precomputed coarse min/max levels — browser-direct, or a Tauri Rust sidecar), not a slicing server. **Trigger to revisit:** the high-rate channels (ECG 100 Hz, future EEG 256 Hz) and especially the **snore spectrogram (~1.85 GB/night, can't be loaded whole)**. Any future server is a **thin, separate, optional** TS data API (`/window`, `/spectrogram`, `/meta`, `/events`) — the frontend stays a static, browser-direct, offline/PWA/webview-wrappable bundle. **Not Next.js / SSR** (conflicts with S10). | Leon |
| O11 | **Entry-point UX** | How a user launches a recording end-to-end (batch run? TS app invokes C++ ingest / triggers Python re-processing as subprocesses?) and what the first-run experience is. | Leon |
| O12 | **LM threshold recalibration (all three of them)** | `threshold=8` (the LM floor, still on the retired uint8 scale), plus **S11**'s `max_threshold=500` mg and `tilt_threshold_deg=10°` — the latter two tuned against a single 2 h recording. Needs a multi-night corpus with scored ground truth. The rotation threshold is the safest of the three (bimodal separation, wide valid range); the amplitude ceiling is the most likely to reject a genuine vigorous LM. Until then, treat every LM/PLMI number as **uncalibrated**. | Dmitry |
| O13 | **Should GPCs surface in `events.json` / the viewer?** | `count_plm` already returns `gpc_events`/`total_gpcs` and `meta.json.stats` carries the counts, but nothing writes them to `events.json` or draws them. Options: (a) leave them a scoring-internal diagnostic; (b) emit a `position_change` event type (§5.2 vocab) + a viewer overlay, making the exclusions auditable on the chart; (c) treat GPC count as a clinical metric in its own right (position changes/hour is a restlessness measure). Ties into **O6** (event-taxonomy scope). | Dmitry + Leon |

---

## How to use this page

- A new page must not contradict a **settled** decision (S1–S11); if it needs to, that is a reopen —
  edit this log first with a dated rationale, then the page.
- A page that touches an **open fork** must link here rather than picking a side.
- Related context: [architecture](../knowledge/architecture.md),
  [data formats](../knowledge/data-formats.md),
  [signal processing](../knowledge/signal-processing.md),
  [viewer](../knowledge/viewer.md),
  [hardware](../knowledge/hardware.md),
  [pipeline assessment](../planning/pipeline-assessment.md).
