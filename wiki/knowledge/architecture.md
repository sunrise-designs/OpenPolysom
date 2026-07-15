---
title: Component-2 Architecture
domain: knowledge
status: living
updated: 2026-07-08
summary: The canonical reference for ProtoSom's data pipeline after data leaves the device — the four-stage pipeline, the three-layer data model, and the C++ ingest / Python processing / TS web-app language boundary that meets at the Zarr store plus metadata.
---

# Component-2 Architecture

ProtoSom has two components. **Component 1** is the hardware data acquisition (a bespoke PCB,
RPi5 + ESP32-C6, owned by Dmitry). **Component 2** is everything *after* the data leaves the device:
ingest, signal processing, the viewer, and the report. This page is the spine — the canonical
reference for how component 2 is structured. Everything else in the wiki hangs off it.

This is a **proof-of-concept**. It is **not** a regulated medical device, but it voluntarily adopts
[IEC 62304](../standards/coding.md) software discipline: traceability, test coverage, and
reproducibility/auditability. Those three constraints shape every decision below.

> The original design brief and its open questions live in
> [`doc/global_architecture.md`](../../doc/global_architecture.md). This page records the **settled**
> architecture; where something is still undecided it links to [open forks](../state/decisions.md).

---

## 1. The four-stage pipeline

Data flows through four stages, left to right. Each stage has a single responsibility and a
well-defined handoff:

```
   device  ──▶  C++ ingest  ──▶  Python processing  ──▶  the TS web app
  (comp. 1)      (Dmitry)          (Dmitry)                 (Leon)
```

| Stage | Owner | What it does | Reads | Writes |
|---|---|---|---|---|
| **device** | comp. 1 (Dmitry) | Acquires biosignals + audio, emits the [raw anchor](#layer-1--raw-capture-the-raw-anchor) | sensors | EDF+ (biosignals), FLAC (audio) |
| **C++ ingest** | Dmitry | Converts the raw capture into the **raw Zarr** layer; extracts header metadata; may own the [clinical export](#layer-3--clinical-export) | raw anchor | raw Zarr ([working store](#layer-2--working-store)) |
| **Python processing** | Dmitry | The signal processing + ML — the heart of the system | raw Zarr | derived Zarr + `events.json` + `meta.json` |
| **the TS web app** | Leon | Reads the [Zarr boundary](#5-the-zarr-boundary--a-language-neutral-contract) + metadata and presents it — charts, spectrogram, UI | Zarr + metadata | **nothing** (read-only) |

The data the chart needs is **screen-resolution for the visible window**, not the whole recording —
see [§4 the batch + on-demand model](#4-the-batch--on-demand-invocation-model) and
[viewer](viewer.md).

---

## 2. The three-layer data model

ProtoSom keeps **three** representations of a recording. Only the middle one is mutable; the others
are immutable or ephemeral. This separation is what makes the pipeline auditable: you can always
regenerate the working store from the raw anchor and prove the result. ([Format detail](data-formats.md).)

### Layer 1 — Raw capture (the raw anchor)

Whatever the device emits, byte-for-byte: **EDF+** for biosignals (read/written via `edflib`; **BDF+**
for future 24-bit EEG), **FLAC** for audio, time-anchored to the biosignal timeline. It is
**immutable and content-hashed** — the provenance anchor that every derived product is stamped
against. It is **never modified** by any stage of component 2.

The 6-channel EDF+ written by [`src/main.cpp`](../../src/main.cpp) (RPi5 acquisition) is a concrete
raw anchor — Thoracic + Abdomen (LDC1612 RIP belts, nH, 50 Hz), HR (BPM, 1 Hz), RR (ms, 5 Hz),
Flow (SDP800, 50 Hz), HR_Raw (AD8232 ECG ADC, 100 Hz); see the `ChannelInfo` table at
[`src/main.cpp:145`](../../src/main.cpp). The ESP32-C6 wrist device adds an 11-channel EDF+
(AccelX/Y/Z @10 Hz + RR @1 Hz), and the newer ESP32-C6 wrist logger
([`logger.cpp`](../../ESP32-C6-heart-idf/components/logger/logger.cpp)) writes an 11-channel
EDF+ (Thoracic, Abdomen, Flow, ECG, Accel0X/Y/Z, Accel1X/Y/Z, RR).

### Layer 2 — Working store

A **Zarr group per recording** — every dense signal is its own array, chunked along the time axis —
plus two JSON sidecars:

- **`meta.json`** — metadata + the provenance block (what raw anchor, what code version, what params
  produced this).
- **`events.json`** — sparse annotations (PLM/LM events, apnea/hypopnea, snore classifications,
  markers).

Two producers write this layer:

- **raw Zarr** — produced by **C++ ingest** from the raw anchor.
- **derived Zarr** — produced by **Python processing** (filtered signals, feature arrays, the
  audio spectrogram array, etc.).

Per-array Zarr attribute `storage = physical | digital` records the EDF write path:
[`src/main.cpp`](../../src/main.cpp) writes physical doubles via `edfwrite_physical_samples` (e.g.
[`src/main.cpp:286`](../../src/main.cpp)), while the ESP32-C6 `logger.cpp` writes digital via
`edfwrite_digital_samples`. The working store is **regenerable from the raw anchor**, so it can be
deleted and rebuilt at will. It is the only mutable layer and the meeting point of the whole
architecture. ([Zarr v2 + Blosc/zstd rationale](data-formats.md), [decisions](../state/decisions.md).)

> **Note on the current code.** [`src_python/export_zarr.py`](../../src_python/export_zarr.py) reflects
> the earlier single-codebase prototype: Python opens the group with
> `zarr.open_group(..., zarr_format=2)` ([`export_zarr.py:56`](../../src_python/export_zarr.py)),
> creates one array per signal, and writes `meta.json` + `events.json` sidecars following the
> canonical nested schema — `subject`/`recording`/`stats`/`layers`/`provenance`, with the full
> git SHA + dirty flag under `provenance.pipeline.git`. Under the settled architecture the
> **raw** Zarr is produced by C++ ingest and the **derived** Zarr + metadata by Python processing;
> the Python writer above is the seed of the derived-layer producer.

### Layer 3 — Clinical export

**EDF+/BDF+ regenerated on demand** for interop with tools like EDFBrowser. It is **never stored** —
it is produced from the derived layer when a clinician asks, then handed off. The export scrubs the
EDF+ header patient name/DOB to produce a shareable de-identified file. May be owned by C++ ingest.

```
 ┌──────────────────┐      ┌────────────────────────┐      ┌─────────────────────┐
 │ Layer 1: RAW      │      │ Layer 2: WORKING STORE  │      │ Layer 3: CLINICAL   │
 │ EDF+ / FLAC       │─────▶│ Zarr arrays + JSON      │─────▶│ EDF+/BDF+ on demand │
 │ immutable, hashed │ regen│ mutable, regenerable    │ regen│ never stored        │
 │ the raw anchor    │◀─────│ the working store       │      │ de-identified       │
 └──────────────────┘ never └────────────────────────┘      └─────────────────────┘
       provenance       raw=C++ ingest · derived=Python proc.    interop / share
```

---

## 3. The language boundary

The authoritative split is **three languages**, each chosen for what it is best at. The governing
principle:

> **C++ ingests, Python processes, TypeScript presents. The three meet at the Zarr store + metadata.**

| Layer | Language | Owner | Responsibility | Reads | Writes |
|---|---|---|---|---|---|
| **C++ ingest** | C++ (edflib + TensorStore/z5) | Dmitry | Device acquisition + convert raw EDF+/FLAC → **raw Zarr**; extract header metadata; may own clinical export | raw anchor | raw Zarr |
| **Python processing** | Python (scipy/numpy/scikit/PyTorch) | Dmitry | Signal processing + ML: baseline removal, AASM PLM/LM, HRV, airflow filtering, apnea/hypopnea, snore VOTE/MFCC, … | raw Zarr | derived Zarr + `events.json` + `meta.json` |
| **the TS web app** | TypeScript (zarrita.js + ECharts/uPlot) | Leon | Charts, spectrogram, UI, the slicing server | Zarr + metadata | **never writes Zarr** |

**Why these three:**

- **C++ for ingest** — performance and medical-compliance friendliness, and it reuses the existing
  `edflib`-based firmware ([`src/main.cpp`](../../src/main.cpp)). Writes Zarr via a C++ Zarr library
  (TensorStore / z5 / xtensor-zarr).
- **Python for processing** — there is a *copious* amount of feature-finding work ("finding salient
  features in the signals"), and Python is the de-facto standard for it. The current processing lives
  in [`src_python/signal_processing.py`](../../src_python/signal_processing.py)
  (`remove_baseline`, `count_plm` for AASM PLM, `compute_hrv` for RMSSD). Reads raw Zarr → writes the
  derived layer + metadata (including provenance). See [signal processing](signal-processing.md).
- **TypeScript for the web app** — reads the [Zarr boundary](#5-the-zarr-boundary--a-language-neutral-contract)
  via `zarrita.js` and presents it ([`src_web/src/main.ts`](../../src_web/src/main.ts) loads `meta`
  then the Zarr arrays and renders with ECharts; the current loader uses `zarr.js` via
  [`src_web/src/zarr_loader.ts`](../../src_web/src/zarr_loader.ts)). It **never** writes Zarr.

### Long-term watch-item

How much Python is acceptable for medical-grade software in the long run (Dmitry). C++ wins on
performance and compliance, and modern C++23 can make scientific code terse/"pythonic". Python is the
**prototyping** choice now; processing **may** migrate Python → C++ later. This is documented as a
**risk, not a now-change** — see [decisions](../state/decisions.md).

---

## 4. The batch + on-demand-invocation model

**Default — batch.** C++ ingest and Python processing run as a batch: device → raw Zarr → derived
Zarr + metadata. Then the TS web app reads and displays.

**On demand.** The TS side may invoke C++ binaries (ingest / export) or trigger Python re-processing
as subprocesses. These write the **derived layer**; the **raw anchor stays immutable**; **each derived
product is stamped with provenance** (the full git SHA + dirty flag + raw-anchor content hash already
written by [`export_zarr.py`](../../src_python/export_zarr.py) into `meta.json.provenance`).

**The viewer reads the Zarr boundary.** At PoC scale the browser reads the whole (tiny) recording
directly via `zarrita.js`. As data grows, a **thin TS slicing server** reads the Zarr and serves the
browser **screen-resolution windows** over an HTTP windowed API
(`/window?start&end&channels&res`, `/spectrogram`, `/meta`, `/events`) — the same reading code, just
moved from browser to server. The server decimates on the fly; precomputed pyramids slot in **later
without changing the viewer**. Audio gets a separate spectrogram pane (`wavesurfer.js`) fed a
Python-precomputed spectrogram array stored in Zarr. See [viewer](viewer.md).

The current end-to-end path is visible in [`export_zarr.py`](../../src_python/export_zarr.py):
`save_zarr_json` writes the Zarr + `meta.json` + `events.json`, and `serve_and_open` starts a local
threaded HTTP server that serves `index.html` + the built chart bundle + `src_web/` static assets
alongside the output, opening the viewer at `index.html?meta=<file>`.

```
        default: batch
        ┌───────────────────────────────────────────────────────────────┐
        │ device ──▶ C++ ingest ──▶ raw Zarr ──▶ Python proc. ──▶ derived │
        │            (raw Zarr)                  (derived Zarr + meta)     │
        └───────────────────────────────────────────────────────────────┘
                                          │ read (zarrita.js)
                                          ▼
                                  the TS web app (viewer)

        on demand: the TS web app spawns C++ (ingest/export) or triggers
        Python re-processing as subprocesses → writes the DERIVED layer only.
        Raw anchor stays immutable; each derived product is provenance-stamped.
```

---

## 5. The Zarr boundary — a language-neutral contract

> **The boundary:** Zarr files + metadata are the boundary between Dmitry's side (C++ ingest +
> Python processing) and Leon's side (the TS web app).

Because **three** languages read and write it, the Zarr format must be a **language-neutral contract**:

- **C++ writes** it (TensorStore / z5).
- **Python reads + writes** it (`zarr-python`).
- **TypeScript reads** it (`zarrita.js`).

This is why the spec is pinned (full detail + rationale in [data formats](data-formats.md)):

- **Zarr v2** + **Blosc** codec (`zstd`, `shuffle`).
- **No Python-only `numcodecs` filters** (no Delta, no PackBits) — they would not decode in C++ or TS.
- v3 only if **every** chosen library supports it; `z5` is v2-only, so the default is **v2**.

---

## 6. The whole picture

```
 COMPONENT 1              COMPONENT 2 — Dmitry's side (C++ + Python)            Leon's side (TS)
 ───────────              ──────────────────────────────────────────          ────────────────

  device                C++ ingest          Python processing                 the TS web app
 ┌────────┐  EDF+/FLAC  ┌──────────┐  raw   ┌─────────────────┐               ┌──────────────┐
 │ sensors│ ──────────▶ │ EDF/FLAC │  Zarr  │ baseline · PLM/LM│  derived      │ slicing server│
 │ RPi5 + │  raw anchor │  → raw   │ ─────▶ │ HRV · airflow ·  │  Zarr +       │ /window       │
 │ ESP32  │ (immutable, │  Zarr +  │        │ apnea · snore ·  │  events.json  │ /spectrogram  │
 └────────┘  hashed)    │  header  │        │ spectrogram      │  meta.json    │ /meta /events │
      │                 │  meta    │        └─────────────────┘     │         └──────┬───────┘
      │  Layer 1        └────┬─────┘  zarr-python (reads + writes)   │  reads only    │ zarrita.js
      └─────────────────────┴─────────── Layer 2: working store ────┴────────────────┘ (browser
                            ╔══════════════════════════════════════════╗               or server)
                            ║  THE ZARR BOUNDARY (language-neutral)      ║
                            ║  Zarr v2 · Blosc(zstd,shuffle) · no PY-only║   ◀── C++ writes
                            ║  filters · + events.json + meta.json       ║   ◀── Python reads+writes
                            ╚══════════════════════════════════════════╝   ◀── TS reads

  Layer 3: EDF+/BDF+ clinical export regenerated on demand (EDFBrowser interop), scrubbed, never stored.
```

---

## Related pages

- [Data formats](data-formats.md) — the raw anchor, working store, and clinical export in detail
  (EDF+/BDF+, Zarr v2 + Blosc, FLAC, sidecar schemas).
- [Signal processing](signal-processing.md) — the Python processing stage (PLM/LM, HRV, snore, …).
- [Viewer](viewer.md) — the TS web app, decimation, slicing server, charting choice.
- [Hardware](hardware.md) — component 1: the acquisition devices that emit the raw anchor.
- [Concepts](concepts.md) — domain glossary (PSG, AASM scoring, the signals and disorders).
- [Coding standards](../standards/coding.md) — the IEC 62304 traceability/test/reproducibility posture.
- [Pipeline assessment](../planning/pipeline-assessment.md) — risks and the packaging trade-offs.
- [Decisions & open forks](../state/decisions.md) — what is settled and what is still undecided.
- [`doc/global_architecture.md`](../../doc/global_architecture.md) — original spec and open questions.
