---
title: Coding Standards
domain: standards
status: living
updated: 2026-07-08
summary: How code is written per language across the three-language boundary — C++ ingest, Python processing, the TS web app — plus the round-trip and reproducibility tests that hold them honest.
---

# Coding Standards

ProtoSom is a proof-of-concept, not a regulated device, but it aspires *voluntarily* to
IEC 62304 software discipline: traceability, test coverage, and
reproducibility/auditability. This page says how code is written in each of the three
languages, and how the **Zarr boundary** keeps them honest. It is the per-language
companion to the [architecture](../knowledge/architecture.md) overview and the
[data formats](../knowledge/data-formats.md) page.

## The three-language boundary (restated)

The single organizing principle of the codebase:

> **C++ ingests, Python processes, TypeScript presents. The three meet at the Zarr store + metadata.**

| Language | Owner | Role | Writes | Reads |
|---|---|---|---|---|
| **C++** | Dmitry | **C++ ingest** — device acquisition + raw EDF+/FLAC → **raw anchor** Zarr; header metadata; may own the [clinical export](../knowledge/data-formats.md) | raw Zarr (TensorStore / z5) | EDF+ via edflib |
| **Python** | Dmitry | **Python processing** — the signal processing + ML | **derived layer** Zarr + `events.json` + `meta.json` | raw Zarr (zarr-python) |
| **TypeScript** | Leon | **the TS web app** (viewer) + **the slicing server** | nothing — **NEVER writes Zarr** | Zarr via zarrita.js |

The **Zarr store + JSON metadata is the boundary** — the membrane between Dmitry's side
(C++ ingest + Python processing) and Leon's side (the TS web app). It is a
*language-neutral contract*: the spec is **Zarr v2 + Blosc (zstd, shuffle)**, with **no
Python-only numcodecs filters** (no Delta/PackBits), so all three libraries can read it.
See [data formats](../knowledge/data-formats.md) for the full contract; the v2/Blosc
choice and the Python→C++ long-term watch-item are recorded in
[decisions](../state/decisions.md).

---

## C++ — the C++ ingest (Dmitry)

The acquisition side already exists: [`src/main.cpp`](../../src/main.cpp) (RPi5, 6-channel)
and `ESP32-S3-heart/.../logger.cpp` (wrist, 4-channel) write EDF+ via **edflib**. C++ also
converts that raw device data into the **raw anchor** Zarr and may own the EDF/BDF
**clinical export**. C++ is chosen for performance and medical-compliance friendliness.

- **Reuse edflib, don't reinvent it.** All EDF+/BDF+ read and write goes through edflib
  (`src/main.cpp:15` `#include "edflib.h"`); do not hand-roll the header. The same library
  backs the clinical export.
- **Deterministic and explicit.** Channel layout is a static table, not runtime magic —
  `src/main.cpp:145-152` declares each channel's label, transducer, sample rate, and
  digital/physical ranges literally. New channels are added to that table; nothing is
  inferred. Record the physical/digital storage choice per array as a Zarr attr
  `storage=physical|digital`: `main.cpp` writes **physical** doubles via
  `edfwrite_physical_samples` (`src/main.cpp:286-291`); `logger.cpp` writes **digital** ints
  via `edfwrite_digital_samples`. Keep the attr and the writer in lockstep.
- **Provenance at the source.** The equipment string carries the build:
  `EQUIPMENT = "OpenPolysom v0.1 (" GIT_COMMIT_HASH ")"` (`src/main.cpp:34`), with
  `GIT_COMMIT_HASH` compiled in. Ingest copies device header metadata (start datetime,
  per-channel ranges, equipment) into `meta.json` so every derived product traces back to
  the firmware that produced it.
- **Performance is why C++ owns ingest.** Tight per-sample loop (`src/main.cpp:204-307`),
  monotonic timing (`clock_gettime(CLOCK_MONOTONIC, …)`), fixed-size stack buffers, periodic
  flush every 10 s (`FLUSH_INTERVAL_SAMPLES`). Keep allocation out of the hot path.
- **The Zarr writer.** raw anchor → raw Zarr is written by C++ using a C++ Zarr library —
  **TensorStore** or **z5** (or xtensor-zarr). z5 is **v2-only**, which is exactly why the
  contract defaults to **Zarr v2**; do not adopt a v3-only feature unless every chosen
  library (incl. zarrita.js on the TS side) supports it.
- **Build / style.** Modern C++ (C++17/23 — C++23 can make scientific code terse and
  "pythonic"), warnings-as-errors, no undefined behaviour in the sample loop. Keep the
  acquisition binary and the ingest/export binary small and invokable as subprocesses by
  the TS web app on demand.

The raw anchor C++ produces is **immutable and content-hashed** — never modified, always
regenerable from the layer below it (the original EDF+/FLAC). See
[data formats](../knowledge/data-formats.md) for the layer model.

---

## Python — the Python processing (Dmitry)

Python owns the signal processing and ML — and there is a **copious** amount of it
("finding salient features in the signals"): baseline removal, AASM PLM/LM, HRV, airflow
filtering, apnea/hypopnea detection, snore VOTE/MFCC classification, and more. Python is
the de-facto standard for this work (scipy/numpy/scikit/PyTorch). Current code lives in
[`src_python/signal_processing.py`](../../src_python/signal_processing.py); see the
algorithm narratives in [signal processing](../knowledge/signal-processing.md).

- **Functional core, imperative shell.** Processing functions are pure transforms over
  arrays — input arrays → output arrays + event lists, no hidden state. `count_plm`,
  `compute_hrv`, `remove_baseline`, `accel_magnitude`
  ([`signal_processing.py`](../../src_python/signal_processing.py)) all take data in and
  return results out. Push I/O — reading the raw Zarr, writing the derived Zarr +
  `events.json` + `meta.json` — to the edges (`read_log.py`, `export_zarr.py`). The
  reader/writer shell is thin; the science is pure and testable.
- **scipy / numpy, vectorised.** Lean on the de-facto stack: `numpy` arrays,
  `scipy.ndimage.median_filter` for the 30 s baseline window (`signal_processing.py:15`),
  vectorised run-detection via `np.diff` / `np.where` (`signal_processing.py:41-43`). Avoid
  Python-level per-sample loops in hot paths; the HRV sliding window is an O(N) two-pointer
  (`signal_processing.py:120-130`), not O(N²).
- **Determinism for auditability.** Same input + same pinned env ⇒ **identical derived
  values**. No wall-clock, no unseeded RNG, no thread-order-dependent reductions. Algorithm
  constants are named and visible — e.g. the AASM windows `MIN_DUR` / `MAX_DUR` / `MIN_GAP`
  / `MAX_GAP` (`signal_processing.py:45-55`) and the ≥4-LM series rule — not buried magic, so
  a reviewer can trace a result back to the scoring rule it implements.
- **Pinned dependencies.** The processing env is pinned (conda-forge lockfile / pinned
  `requirements`), and the pin is part of the provenance: `meta.json` records the env hash so
  a derived product can be regenerated. This is what makes the reproducibility test below
  meaningful.
- **Tests: pytest + property + reference.** Unit tests for the pure functions; **property
  tests** for invariants (e.g. every scored PLM series has ≥4 LMs with 5–90 s onset gaps —
  the AASM rule in `count_plm`); **reference tests** that pin known fixtures to expected
  scores (golden values), so an algorithm change that moves a number is caught in review.
- **Output is "Zarr + metadata".** The processing scripts read the **raw anchor** Zarr and
  write the **derived layer** Zarr + `events.json` (sparse annotations) + `meta.json`
  (metadata + provenance). They never touch the immutable raw anchor; each derived product
  is stamped with provenance. They obey the [data formats](../knowledge/data-formats.md)
  contract: Zarr v2, Blosc(zstd, shuffle), **no numcodecs-only filters** — that ban is a hard
  rule because the TS web app must read the same arrays.

> **Reading note.** `remove_baseline` / `count_plm` / `accel_magnitude` take plain
> physical-unit arrays + `fs` directly. `src_python/read_log.py` sources those arrays
> from the EDF+ raw anchor via `edf_reader.py` (`edfio`), which is still short of the
> target of reading the **raw Zarr** (doesn't exist yet — see
> [roadmap](../state/roadmap.md) stage 2).

> **Watch-item:** how much Python is acceptable *long-term* for medical-grade software is
> open. C++ wins on performance + compliance, so processing **may** migrate Python→C++
> later. Python is the **prototyping** choice now; documented as a risk, not a now-change —
> see [decisions](../state/decisions.md).

---

## TypeScript — the TS web app (Leon)

The viewer reads the **Zarr boundary** + metadata and displays it — charts (ECharts /
uPlot), the spectrogram pane (wavesurfer.js), the UI, and the **slicing server** as data
grows. Current code is `src_web/src/*.ts` (`main.ts`, `chart.ts`, `zarr_loader.ts`,
`types.ts`). See [viewer](../knowledge/viewer.md).

- **NEVER writes Zarr.** The TS side is read-only over the boundary. It may *invoke* C++
  binaries (ingest / export) or *trigger* Python re-processing as subprocesses — those write
  the derived layer; the TS code itself never writes store bytes.
- **Reads Zarr via zarrita.js.** Same reading code whether in-browser (PoC scale: load the
  whole tiny recording) or moved into the **slicing server** later (HTTP windowed API:
  `/window?start&end&channels&res`, `/spectrogram`, `/meta`, `/events`). The slicing server
  is the canonical imperative shell: it reads Zarr, calls pure decimation/windowing
  functions, decimates on the fly, and answers the windowed API. zarrita.js is a v2/v3
  reader; the contract stays **v2** so it interops with the z5 / TensorStore writer.
- **Functional, immutable by default.** Pure functions for all logic — decimation, windowing
  math, LTTB — with mutation, I/O, and the DOM/canvas pushed to the edges. Use `readonly` on
  properties, `readonly T[]` / `ReadonlyArray<T>`, `Readonly<T>`, `ReadonlyMap` /
  `ReadonlySet`, and `as const`. `const` only stops rebinding, so immutability is enforced by
  the **linter**, not the compiler alone.
- **strict tsconfig.** `"strict": true` is the non-negotiable baseline.
- **ESLint, all rules at `"error"` (never `"warn"`).** Includes `eslint-plugin-functional`
  (`immutable-data`, `no-let`, `prefer-readonly-type`) plus `@typescript-eslint/prefer-readonly`
  and `@typescript-eslint/prefer-readonly-parameter-types`. Any violation fails the commit.
- **Husky hooks are the gate.** Pre-commit runs ESLint + `tsc --noEmit` on staged files (via
  lint-staged); pre-push runs the full **Vitest** suite. A violation, type error, or failing
  test exits non-zero and blocks the commit/push. No CI fallback — local hooks are the gate.
- **TDD where the spec is clear** (the windowed API contract, decimation invariants, the
  Zarr-read decoding, the round-trip tests below). Prefer property-based and integration tests
  over thin example tests; skip TDD for exploratory UI/UX. Green still wants a human review
  pass for security, performance, and clarity.

---

## Round-trip & reproducibility tests (cross-language)

These are the tests that make "voluntary IEC 62304" real. They are owned wherever the
producing code lives, but they assert *across* the Zarr boundary, and a failure blocks merge
the same way a lint error does.

1. **EDF → Zarr → EDF round-trip (C++ ingest / clinical export).** Ingest a known EDF+ into
   the raw anchor Zarr, regenerate a **clinical export** EDF+/BDF+, and assert the biosignal
   samples and kept header fields recover the original within the documented quantisation
   tolerance (digital codes exact; physical values within the EDF+ digital/physical scaling).
   Catches channel-table, scaling, and `storage=physical|digital` mistakes. The clinical
   export is regenerated on demand and never stored, so this test *is* its specification —
   see [data formats](../knowledge/data-formats.md).

2. **Same input + pinned env ⇒ identical derived values (Python processing).** Run the
   processing pipeline twice — and on a fresh checkout of the pinned env — over the same raw
   Zarr; assert the derived Zarr arrays, `events.json`, and the scored metrics (PLMI, RMSSD,
   …) are **value-for-value identical** (modulo the timestamp/run-id fields excluded from the
   hash). A diff here is a determinism bug. This is the auditability contract: a result is
   reproducible from `meta.json`'s provenance (firmware `GIT_COMMIT_HASH`, env hash,
   raw-anchor content hash).

3. **Cross-language read parity (the Zarr boundary).** A small fixture Zarr written by the
   C++/Python side is read by **zarrita.js** in the TS web app's test suite and asserted equal
   to the values the Python side reads back. This is the regression guard for the
   **no-numcodecs-filter / Zarr-v2 / Blosc(zstd, shuffle)** rule — if someone adds a
   Python-only Delta/PackBits filter, this test goes red because zarrita.js can't decode it.

4. **TS consumer invariants (property-based).** Decimation/windowing on the TS side must never
   alter clinical semantics: the full-range window equals the raw series; a decimated window's
   min/max envelope bounds the underlying samples; LTTB preserves endpoints. The consumer may
   drop pixels, never misrepresent a number.

Where a decision feeding these tests is still open (PDF report runtime, v3 adoption,
Python→C++ migration), link to the [open forks](../state/decisions.md) rather than inventing
a requirement here.

## See also

- [Architecture](../knowledge/architecture.md) — the pipeline, the Zarr boundary, the slicing server.
- [Data formats](../knowledge/data-formats.md) — raw anchor, working store (Zarr v2 + codec rules), clinical export.
- [Signal processing](../knowledge/signal-processing.md) — the algorithms the Python processing implements.
- [Viewer](../knowledge/viewer.md) — what the TS web app renders.
- [Decisions / open forks](../state/decisions.md) — settled choices and what is still undecided.
