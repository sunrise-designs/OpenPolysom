---
title: Build Roadmap
domain: state
status: living
updated: 2026-08-09
summary: The ordered build plan from the current repo state to the target three-language pipeline, with dependencies, exit criteria, and the open forks that gate it.
---

# Build Roadmap

The path from where the repo is **today** to the target pipeline:
**device → C++ ingest → Python processing → the TS web app**, meeting at the
**Zarr boundary** (Zarr v2 + JSON sidecars). For the shape this is building
toward see [architecture](../knowledge/architecture.md); for the choices behind
it and the still-undecided forks see [decisions](../state/decisions.md); for the
review that produced the plan see [the pipeline assessment](../planning/pipeline-assessment.md).

This page is **living** — re-order and tick items as the build moves.

## Where we are today (current state)

- **C++ ingest** writes EDF+ at the device only:
  [`logger.cpp`](../../ESP32-C6-heart-idf/components/logger/logger.cpp)
  (ESP32-C6, 11-channel digital samples) via edflib. There is **no
  EDF+ → raw-Zarr** ingest step yet — Zarr is currently produced by Python.
- **Python processing** ([`src_python/signal_processing.py`](../../src_python/signal_processing.py))
  reads the **EDF+ raw anchor directly** via
  [`edf_reader.py`](../../src_python/edf_reader.py) (`edfio`), not the raw Zarr:
  `remove_baseline`, `count_plm` (AASM PLM/LM, run per accelerometer, with
  Gross-Position-Change rejection per [decisions § S11](../state/decisions.md)),
  `compute_hrv` (RMSSD). Tests are **partial**: `src_python/tests/` covers the GPC
  gate directly and exercises `count_plm` indirectly through the windowed-metrics
  registry, but the AASM duration/gap/series rules, `compute_hrv`, and
  `remove_baseline` still have **no direct tests** of their own.
- **Zarr is written from Python** ([`export_zarr.py`](../../src_python/export_zarr.py),
  `zarr_format=2`, single-chunk arrays, `meta.json` + `events.json` sidecars following
  the canonical schema) — the right *layer*, but written from the wrong stage (Python,
  not C++ ingest) and sourced from EDF+ directly rather than from a raw Zarr.
- **The TS web app** ([`src_web/src/`](../../src_web/src/)) reads the Zarr +
  sidecar and charts it via **zarrita** — stage 3's migration has landed — and
  loads the whole recording in-browser. It also has a second, **RT mode** that
  plots the eleven raw EDF+ channels live from the device's Wi-Fi WebSocket
  (`components/rt_stream`), which is additive to the pipeline rather than part
  of it: nothing is persisted and nothing derived is computed. See
  [decisions § S12](../state/decisions.md). No slicing server, no audio pane, no
  PDF report.

So the spine exists end-to-end, but Python reads the EDF+ raw anchor directly
instead of a raw Zarr and the viewer still uses zarr.js, and the three-layer
model, the language boundary, the tests, and the provenance primitives are
mostly **intentions, not code**. The roadmap below closes that gap.

## Dependency order at a glance

```
0  Settle open forks ──┐
                       ├─► 1  C++ EDF+→raw-Zarr ingest ─► 2  Python processing on raw Zarr
                       │                                        │
                       └─────────────────────────────► 3  TS web app reads the Zarr boundary
                                                                │
   4  Audio (FLAC → Python spectrogram → wavesurfer) ──────────┤
   5  PDF report (open fork) ─────────────────────────────────┤
   6  Provenance / reproducibility primitives  (cuts across 1–5)
   7  Air-gapped packaging (3 languages)        (last; needs 1–5 stable)
```

Stages 1 and 3 can proceed in parallel once the **Zarr boundary spec** is fixed
(stage 0): C++ ingest produces it, the TS web app consumes it, and they only
need to agree on the contract, not on each other's internals. Stage 2 depends on
stage 1 (it reads the raw Zarr). Provenance (6) is woven through 1–5 rather than
bolted on at the end.

---

## 0 — Settle the open forks with Dmitry  *(gates everything)*

Cheap to decide, expensive to discover later. Lock these before writing ingest
code; each is tracked in [decisions](../state/decisions.md) as an [open fork](../state/decisions.md).

- **C++ Zarr library** — TensorStore vs z5 vs xtensor-zarr. Constrains the spec:
  z5 is **v2-only**, which is *why* the boundary is **Zarr v2 + Blosc(zstd,
  shuffle), no Python-only numcodecs filters** (see
  [data formats](../knowledge/data-formats.md)). Pick before stage 1.
- **PDF report language** — Python (matplotlib + pikepdf) vs TS (pdf-lib).
  Gates stage 5 only; can lag the others.
- **Slicing-server trigger** — at what recording size the TS web app stops
  reading Zarr browser-direct and moves to the thin slicing server. Gates the
  *second half* of stage 3, not the first.
- **Long-term Python→C++ watch-item** — how much Python is acceptable long-term
  for medical-grade software. Logged as a **risk, not a now-change**; does not
  block, but frame stage 2 so a later port is cheap (pure functions, clear I/O
  edges per the [coding standards](../standards/coding.md)).

**Exit:** each fork has a recorded decision or an explicit "defer, revisit at
stage N" note in [decisions](../state/decisions.md).

## 1 — C++ EDF+ → raw-Zarr ingest

The first real code. Build the **C++ ingest** step that turns the **raw anchor**
(EDF+ from the device, read via edflib) into the **raw** layer of the
[working store](../knowledge/architecture.md): one Zarr array per dense signal,
chunked along time, plus extracted EDF+ header metadata into `meta.json`.

- Read EDF+ with edflib. Honour the per-array `storage=physical|digital` attr —
  `logger.cpp` writes **digital** (`edfwrite_digital_samples`), so ingested arrays
  are `storage=digital`; see [data formats](../knowledge/data-formats.md).
- Write Zarr v2 with the chosen C++ lib (stage 0), Blosc(zstd, shuffle), one
  array per channel (Thoracic, Abdomen, Flow, ECG, Accel0X/Y/Z, Accel1X/Y/Z, RR),
  each chunked along time — **not** the
  single-chunk shortcut [`export_zarr.py`](../../src_python/export_zarr.py) uses.
- **Round-trip tests** (this is the IEC-62304 entry point): EDF+ → raw Zarr →
  back to EDF+ must reproduce samples bit-exact for digital channels and within
  the physical-resolution tolerance for physical channels. Same for the
  on-demand **clinical export** (EDF+/BDF+ regenerated, never stored). Property
  tests over channel count / rate / dtype, not just one fixture.

**Exit:** a real EDF+ capture ingests to a v2 Zarr that the TS web app can open,
the round-trip test is green, and `meta.json` carries the EDF+ header fields.

**Depends on:** stage 0 (Zarr lib + spec).

## 2 — Port & validate the Python processing against the raw Zarr

Re-point [Python processing](../knowledge/signal-processing.md) at the **raw
Zarr** — it currently reads the EDF+ raw anchor directly (via `edf_reader.py` /
`edfio`) — and put the clinical DSP under test. This is the producer the
assessment flagged as untested.

- **Read raw Zarr → write derived Zarr + `events.json` + `meta.json`** (with a
  provenance block, stage 6). `remove_baseline`, `count_plm`, `compute_hrv`
  already take plain physical-unit arrays + `fs`; the remaining work is
  sourcing those arrays from the raw Zarr instead of reading the EDF+ directly.
- **Tests against the raw Zarr** (TDD per [coding standards](../standards/coding.md)):
  PLM/LM scoring against the AASM rules in
  [`plans/removing accel baseline.md`](../../plans/removing%20accel%20baseline.md)
  (0.5–10 s LM duration, 5–90 s onset-to-onset gap, series ≥4); RMSSD against a
  hand-computed fixture; baseline removal as a property (median-filter
  idempotence / edge behaviour). Pin a golden recording and assert PLMI / HRV
  outputs.
- Keep functions **pure with I/O at the edges** so a later Python→C++ port
  (stage-0 watch-item) is mechanical.

**Exit:** processing runs raw-Zarr → derived-Zarr, the DSP test suite is green
and wired into the pre-push hook, and `events.json` (sparse annotations:
`lm_events`, `plm_groups`) + `meta.json` (stats + provenance) are emitted.

**Depends on:** stage 1 (the raw Zarr it reads).

## 3 — The TS web app reads the Zarr boundary (zarr.js → zarrita.js)

Move [the TS web app](../knowledge/viewer.md) onto the agreed reader and the
windowed read pattern. It **never writes Zarr**.

- ~~**Migrate `zarr` → `zarrita.js`**~~ — **done.** `zarr_loader.ts` uses
  `FetchStore` + `zarr.open`/`zarr.get`. Still owed: confirming it decodes the
  **C++-written** store from stage 1 unchanged, which is the actual
  cross-language integration check and needs stage 1 to exist first.
- **Read the derived Zarr + `meta.json` + `events.json`** from stage 2 and chart
  them (ECharts canvas + LTTB today; uPlot optional). Render `lm_events` /
  `plm_groups` as overlays.
- **Windowed slicing server, when the trigger fires** (stage-0 fork): the same
  reading code moves from browser to a thin TS server exposing
  `/window?start&end&channels&res`, `/spectrogram`, `/meta`, `/events`,
  decimating on the fly. Precomputed pyramids can come later **without changing
  the viewer**. Until the trigger, keep reading browser-direct.

**Exit:** the viewer opens a C++-ingested, Python-processed store via zarrita,
charts every channel for the visible window, and shows PLM/HRV overlays — with
no Python serving the bytes.

> **Landed alongside, not part of this stage: RT mode.** The viewer now also
> reads a live WebSocket from the device (`protosom.rt/1.0.0`) and plots the raw
> EDF+ channels as they are acquired. It does not touch the Zarr boundary and
> does not advance stages 1–2; it is a monitoring view that exists so a bad
> montage is caught during the night rather than the morning after. See
> [viewer § RT vs batch](../knowledge/viewer.md) and
> [decisions § S12](../state/decisions.md).

**Depends on:** stage 0 (spec); reads output of stages 1–2 but the migration
itself can start in parallel against a stage-1 fixture.

## 4 — Audio: FLAC anchor + Python spectrogram + wavesurfer pane

Add the audio path end-to-end. Audio is **not** a line chart — it gets its own
pane fed a precomputed array.

- **FLAC** as the time-anchored raw audio sidecar (the proposed proprietary
  binary is dropped — see [data formats](../knowledge/data-formats.md)).
- **Python processing computes the spectrogram** and stores it as a Zarr array
  in the derived layer (snore VOTE/MFCC classification can follow once the pane
  exists).
- **wavesurfer.js spectrogram pane** in the TS web app, reading that Zarr array
  via the same boundary.

**Exit:** a recording with audio shows a spectrogram pane time-aligned to the
biosignal charts.

**Depends on:** stages 2 (producer) and 3 (reader).

## 5 — PDF report  *(open fork)*

A shareable report. Language is **undecided** — pinned in stage 0.

- **If Python:** matplotlib + pikepdf, reusing the [Python processing](../knowledge/signal-processing.md)
  runtime, `rcParams['pdf.fonttype']=42` for selectable text + a real
  bookmark/outline tree.
- **If TS:** pdf-lib, reusing the web app's chart rendering.
- Either way, the report must carry the **provenance block** (stage 6) and the
  **clinical export** must scrub the EDF+ header patient name/DOB for a
  de-identified file (see PII handling in [decisions](../state/decisions.md)).

**Exit:** a report PDF with selectable text, an outline tree, the charts, and
stamped provenance.

**Depends on:** stage 3 (charts) or stage 2 (Python rendering); fork from stage 0.

## 6 — Provenance & reproducibility primitives  *(cuts across 1–5)*

The IEC-62304 backbone: a result must trace to the exact inputs and code that
made it. Designed into the three-layer model; build it incrementally as the
stages land, not as a final pass.

- **Content-hash the raw anchor** (immutable EDF+/FLAC) on ingest (stage 1) and
  record the hash in `meta.json`.
- **Stamp every derived product** (Zarr + `events.json` + `meta.json`, stages
  2/4/5) with a provenance block: the raw-anchor hash, the **full git SHA** (the
  current `git_hash` in [`export_zarr.py`](../../src_python/export_zarr.py) is
  *short* — widen to the full SHA + dirty flag), the pinned env, and a UTC
  timestamp.
- **Pin the environments** — Python via the stage-7 lockfile/wheelhouse, C++ via
  recorded toolchain + lib versions, TS via the bundle lockfile — so a derived
  product is regenerable from the raw anchor.

**Exit:** any derived product names its raw-anchor hash, full git SHA, and env;
re-running ingest+processing on the same raw anchor reproduces the derived layer.

## 7 — Air-gapped packaging (three languages)  *(last; heaviest)*

Honestly the heaviest part, **because** there are three runtimes. Do it last,
when 1–5 are stable, per OS (Win/Linux/Mac).

- **C++ binaries** — compiled and bundled per-OS (ingest + clinical export).
- **Python env** — conda constructor pinned to conda-forge, **or** a vendored
  wheelhouse via `pip --no-index`.
- **TS web app** — prebuilt static bundle + the thin slicing server (Node, or a
  single binary via Bun/Deno/Node SEA).
- **macOS:** notarization only for a frictionless web download; air-gapped
  installs use `xattr -cr`. Document the per-OS cost honestly.

**Exit:** a clean air-gapped machine on each OS can ingest → process → view a
recording with no network.

**Depends on:** stages 1–5 stable; consumes the pins from stage 6.

---

## Cross-cutting, always-on

- **Tests gate commits/pushes.** Pre-commit runs ESLint + `tsc --noEmit`;
  pre-push runs the suite. New stages land with tests, not after — see
  [coding standards](../standards/coding.md).
- **Raw anchor is sacred.** Nothing in any stage mutates layer 1; everything
  downstream is regenerable. The membrane stays files + subprocess, no FFI.
- **Keep the boundary neutral.** No Zarr v3 array unless *every* chosen lib reads
  it; no Python-only numcodecs filter — either silently breaks a consumer
  (see [data formats](../knowledge/data-formats.md)).

## Related

- [architecture](../knowledge/architecture.md) — the target structure
- [decisions](../state/decisions.md) — settled choices and the open forks gating stage 0
- [pipeline assessment](../planning/pipeline-assessment.md) — the review behind this plan
- [data formats](../knowledge/data-formats.md) — the Zarr boundary spec & three-layer model
- [signal processing](../knowledge/signal-processing.md) — the DSP under test in stage 2
- [the TS web app](../knowledge/viewer.md) — the reader in stage 3
- [hardware & C++ ingest](../knowledge/hardware.md) — the devices feeding stage 1
- [coding standards](../standards/coding.md) — TDD + functional discipline the stages follow
