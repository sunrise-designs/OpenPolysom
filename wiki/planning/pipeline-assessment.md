---
title: Pipeline Architecture Assessment
domain: planning
status: snapshot
updated: 2026-06-19
summary: The 2026-06-19 assessment that produced the settled pipeline — verdict, charting/PDF/packaging answers, the four top risks, and how the three-language boundary settled.
---

# Pipeline Architecture Assessment

A point-in-time record of the review that turned the original four-question
sketch in [`doc/global_architecture.md`](../../doc/global_architecture.md) into
the settled pipeline. It captures the verdict, the concrete answers to the
charting / PDF / packaging questions, the risks that drove the design, and how
the language boundary landed. For the resulting structure see
[architecture](../knowledge/architecture.md); for the choices and their open
forks see [decisions](../state/decisions.md).

This is a snapshot. It is not maintained as the design evolves.

## What was assessed

The original sketch (`doc/global_architecture.md:7-12`) proposed: device →
SD card in a **proprietary binary** → **Python ingest** to Zarr + JSON →
**Python signal processing** → TypeScript viewer (ECharts) served over **HTTP
Range Requests**. It then asked four questions: (1) is this sane? (2) is ECharts
the best charting option? (3) what is the best PDF-report approach? (4) can the
whole thing be packaged for air-gapped Win/Linux/Mac?

## Verdict: the pipeline is sane

Yes. The spine — **device → ingest → signal processing → web app (viewer)**
with the **raw capture preserved for auditability** (`doc/global_architecture.md:10`)
— is the right shape and survives intact. The assessment kept the four-stage
flow and the principle that derived products never overwrite raw capture. What
changed is everything underneath: the formats, the language boundary, and how
the viewer actually gets its data. See [architecture](../knowledge/architecture.md)
and the [three-layer data model](../knowledge/architecture.md).

## Answers to the open questions

- **Charting (Q2).** ECharts is fine and is retained: canvas rendering plus LTTB
  downsampling is the documented large-line path. uPlot (MIT, ~50 KB) is noted
  as a lighter, maintainability-friendly alternative, not a required swap. Audio
  gets a separate spectrogram pane (wavesurfer.js) fed a **Python-precomputed
  spectrogram array stored in Zarr** — not the line-chart path. See
  [viewer](../knowledge/viewer.md).
- **PDF report (Q3).** The raster-charts-into-a-structured-PDF instinct was
  sound; sharpened to: a real bookmark/outline tree + selectable text. Left as
  an **open fork** — either **Python** (matplotlib + pikepdf, reusing the
  [Python processing](../knowledge/architecture.md) runtime,
  `rcParams['pdf.fonttype']=42`) or **TS** (`pdf-lib` reusing the web app's
  chart rendering). Undecided; see [decisions](../state/decisions.md).
- **Packaging (Q4).** Realistic, and honestly the **heaviest** part of the
  project, *because* there are three languages: **C++ binaries** (compiled,
  bundled per-OS), a **Python env** (conda constructor pinned to conda-forge, or
  a vendored wheelhouse via `pip --no-index`), and the **TS web app** (prebuilt
  static bundle + a thin server — Node, or a single binary via Bun/Deno/Node
  SEA). Each is individually feasible; the per-OS cost is real and documented
  honestly. macOS notarization is only needed for frictionless web download;
  air-gapped installs use `xattr -cr`. See [packaging](packaging.md).

## How the language boundary settled

The review's central structural decision was the **three-language split**, with
a single language-neutral contract between the two sides:

- **C++ ingest** (Dmitry) — device acquisition (already exists:
  [`src/main.cpp`](../../src/main.cpp) on the RPi5,
  [`logger.cpp`](../../ESP32-C6-heart-idf/components/logger/logger.cpp) on the
  ESP32-C6, both writing EDF+ via edflib) + converting raw EDF+/FLAC into the
  **raw** Zarr layer + header-metadata extraction. Writes Zarr via a C++ Zarr
  lib (TensorStore / z5 / xtensor-zarr); may own the EDF/BDF clinical export.
- **Python processing** (Dmitry) — the copious signal processing + ML
  (baseline removal, AASM PLM/LM, HRV/RMSSD, airflow filtering, apnea/hypopnea,
  snore VOTE/MFCC), the de-facto standard via scipy/numpy/scikit/PyTorch. Reads
  the raw Zarr → writes the **derived** Zarr + `events.json` + `meta.json`
  (incl. provenance). This is a first-class **producer**, not offline-only.
- **The TS web app** (Leon) — reads the Zarr boundary + metadata and presents
  it (charts, spectrogram, UI, the [slicing server](../knowledge/viewer.md)).
  **Never writes Zarr.** May invoke C++ binaries (ingest/export) or trigger
  Python re-processing on demand.

**The Zarr store + JSON metadata is the boundary** between Dmitry's side (C++
ingest + Python processing) and Leon's side (the TS web app): C++ writes
(TensorStore/z5), Python reads+writes (zarr-python), TS reads (zarrita.js). That
three-way neutrality forces the spec — **Zarr v2 + Blosc(zstd, shuffle), no
Python-only numcodecs filters** (Delta/PackBits) — because z5 is v2-only and a
Python-only codec would be unreadable from C++/TS. Principle: *"C++ ingests,
Python processes, TypeScript presents. The three meet at the Zarr store +
metadata."* See [data formats](../knowledge/data-formats.md).

## Top risks the assessment surfaced

These four problems are what the settled [decisions](../state/decisions.md)
exist to address.

1. **Three-format / three-language mess.** The sketch implied a proprietary
   binary *plus* Zarr all in flight, with no single provenance anchor; and
   three languages must now agree on the Zarr contract. Resolved by collapsing
   formats to a clear three-layer model — **EDF+/BDF+ as the raw anchor**, the
   **working store** (Zarr v2 + JSON sidecars), a regenerated-on-demand
   **clinical export** — and by pinning Zarr v2 + Blosc and forbidding any
   language-specific filter. The proprietary binary is dropped. Any drift
   (a v3 array a v2-only lib can't read, a Python-only codec) silently breaks a
   consumer, so this remains the standing integration hazard and the reason
   packaging is heavy. See [data formats](../knowledge/data-formats.md).

2. **Responsiveness was vapor.** "HTTP Range Requests" (`doc/global_architecture.md:11`)
   does not make a chart responsive — a Range request over a Zarr store still
   ships raw chunks, and the browser would have to decode much of the recording
   to draw a few thousand pixels. Replaced with: read Zarr **directly
   in-browser** at PoC scale, and as data grows a thin **slicing server** serving
   *the data the chart needs* (screen-resolution for the visible window) via a
   windowed API (`/window`, `/spectrogram`, `/meta`, `/events`), decimating on
   the fly, with multi-resolution pyramids addable later without touching the
   viewer. The replacement is a plan, not yet built. See
   [viewer](../knowledge/viewer.md) and [the membrane](../knowledge/architecture.md).

3. **Zero tests against IEC-62304 aspirations.** The project aspires
   *voluntarily* to medical-software discipline — traceability, test coverage,
   reproducibility/auditability — but the clinical DSP
   ([`src_python/signal_processing.py`](../../src_python/signal_processing.py):
   PLM scoring, RMSSD, baseline removal) has no tests, even though the spec
   already calls processing the part that "must be tightly version controlled,
   unit tested, and auditable" (`doc/global_architecture.md:10`). Test coverage
   of the [Python processing](../knowledge/architecture.md) producer is an open
   obligation, not a solved one. See [decisions](../state/decisions.md).

4. **Absent reproducibility primitives.** Nothing content-hashed the raw
   capture, and nothing stamped derived products with provenance, so a result
   could not be traced back to the inputs and code that made it. The fix is
   designed into the data model — the **raw anchor is immutable and
   content-hashed**, the derived layer is **regenerable from layer 1**, and every
   derived product carries a provenance block in `meta.json` — but it is not yet
   implemented, so those guarantees are currently intentions. See
   [the three-layer data model](../knowledge/architecture.md).

A standing **long-term watch-item** (Dmitry): how much Python is acceptable
long-term for medical-grade software. C++ wins on performance + compliance;
modern C++23 can make scientific code terse. Python is the **prototyping** choice
now; processing *may* migrate Python→C++ later — logged as a risk, not a
now-change.

## What this assessment locked in

- The **three-language boundary**: **C++ ingests, Python processes, TypeScript
  presents.** C++ ingest and Python processing both produce Zarr; the TS web app
  only reads it, never writes store bytes. See [decisions](../state/decisions.md).
- The **membrane**: the sides meet only at the Zarr + JSON files plus subprocess
  invocation (TS may invoke C++ / trigger Python) — no FFI. See
  [the membrane](../knowledge/architecture.md).
- Zarr **v2** (not v3) with a Blosc(zstd, shuffle) codec and **no Python-only
  filters**, to keep the C++ and TS readers in play. See
  [data formats](../knowledge/data-formats.md).
