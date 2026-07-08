---
title: Signal Processing (Python)
domain: knowledge
status: living
updated: 2026-07-08
summary: The Python processing stage — baseline removal, AASM PLM/LM, HRV/RMSSD — documented as the executable spec, with the future apnea / EEG / snore roadmap and the Python-vs-C++ watch-item.
---

# Signal Processing (Python)

This is the **Python processing** stage of the [pipeline](architecture.md): the step that turns the immutable [raw anchor](data-formats.md) into salient clinical features. In the [authoritative three-language split](architecture.md), *C++ ingests, Python processes, the TS web app presents, and the three meet at the [Zarr boundary](data-formats.md) + metadata.* Python's contract is: **read the raw Zarr → write the derived Zarr arrays + `events.json` + `meta.json` (with provenance)**. There is a **copious** amount of it — "finding salient features in the signals" is the heart of the project — which is why Python (scipy / numpy / scikit-learn / PyTorch) is the de-facto choice for this stage.

See also: [architecture](architecture.md) · [concepts](concepts.md) · [data formats](data-formats.md) · [hardware](hardware.md) · [viewer](viewer.md) · [open forks / decisions](../state/decisions.md).

---

## What ships, what's reference

Two things sit on this page and they are not the same thing — keep them separate:

- **The clinical algorithm** (the AASM rules, the HRV definition). This is the spec to preserve forever. Tests assert on **scoring results**, not on byte layouts.
- **The current Python code** ([`src_python/signal_processing.py`](../../src_python/signal_processing.py)). This is the **executable specification** and the reference implementation. It is also the runtime for [ML training](#future-roadmap-undecided-and-unbuilt). It is **not** the air-gapped shipping path on its own — see the [watch-item](#long-term-python-vs-c-watch-item).

`remove_baseline`/`count_plm`/`accel_magnitude` take plain physical-unit arrays + `fs`
directly; `src_python/read_log.py` sources those arrays from the EDF+ raw anchor via
`edf_reader.py` (`edfio`) — still a shortcut ahead of the target of reading the
[working store](data-formats.md) Zarr arrays (see [roadmap](../state/roadmap.md) stage 2).

---

## Stage contract (the Zarr boundary)

| | |
|---|---|
| **Reads** | raw [working store](data-formats.md) Zarr arrays (per-signal, chunked along time), each carrying its `storage=physical\|digital` and `fs` attrs |
| **Writes** | derived Zarr arrays (e.g. baseline-removed signals, vector-magnitude, the audio spectrogram array) + `events.json` (sparse annotations: LM/PLM, apnea, arousal events) + `meta.json` (metadata + provenance) |
| **Never** | mutates the raw anchor; writes the clinical export (that may be [C++](architecture.md)); reads/writes the retired `.bin` format |
| **Provenance** | every derived product is stamped (input content-hash, code version, parameters) so a result is reproducible and auditable — the [IEC 62304](../standards/coding.md) aspiration |

Default execution is a **batch**: device → C++ ingest (raw Zarr) → Python processing (derived Zarr + meta) → the TS web app reads + displays. On demand the TS side may trigger Python re-processing as a subprocess; it writes the derived layer, the raw anchor stays immutable. See [architecture › the membrane](architecture.md).

---

## Current implementations (the spec)

All three live in [`src_python/signal_processing.py`](../../src_python/signal_processing.py). Line cites are to that file.

### 1. Baseline removal — rolling-median subtraction

`remove_baseline(channels, window_sec=30, fs=50)` (`:5`).

**Intent.** Strip slow baseline drift / posture step-changes from the accelerometer channels while **preserving fast jerks** (the thing PLM scoring depends on). A rolling **median** (not mean) is used precisely because it rejects the brief high-amplitude jerks rather than smearing them into the baseline.

**Algorithm.**
1. Takes any number of 1-D array-likes, already in physical units (e.g. mg from the EDF+'s Accel0X/Y/Z or Accel1X/Y/Z channels) (`:5-9`).
2. For each channel, `filtered = ch − median_filter(ch, size=window, mode='reflect')` where `window = window_sec * fs` samples (`:11-14`). For the default 30 s @ 50 Hz (the ESP32-C6 accel rate) that is a **1500-sample** sliding median window.
3. `mode='reflect'` at the edges — chosen to **avoid one-sided lag artifacts** at the recording boundaries, instead of zero-padding which would pull the first/last window toward zero.

**Spec invariants (what a port must hold):**
- Subtracting a **median**, not a mean (jerk-preserving).
- Window length is `window_sec * fs` samples and configurable.
- Edge handling reflects (no boundary lag).
- A constant DC offset on the input vanishes from the output (baseline subtraction removes any constant).

**Future** — the chest-belt ([RIP](concepts.md)) baseline is a harder problem and a separate [open fork](../state/decisions.md): the [`README.md`](../../README.md) plan is **airPLS** (Adaptive Iteratively Reweighted Penalized Least Squares) combined with a **QDC** (Qualitative Diagnostic Calibration, Sackner 1989) variant — not the accelerometer's rolling median. Document, don't build yet.

### 2. AASM PLM / LM detection

`count_plm(ax, ay, az, threshold=8, fs=50)` (`:17`). Periodic Limb Movement scoring per the **AASM Scoring Manual v2.6** (rules transcribed in [`plans/removing accel baseline.md`](../../plans/removing%20accel%20baseline.md)). Runs once per accelerometer — the ESP32-C6 logger has two (Accel0, Accel1) — scored and reported separately; see [current state](../state/current.md).

**AASM rules implemented (the spec — never silently change these):**

| Criterion | Rule | In code |
|---|---|---|
| LM amplitude | vector magnitude ≥ `threshold` above baseline | `:23` |
| LM min duration | ≥ 0.5 s (25 samples @ 50 Hz) | `MIN_DUR`, `:28` |
| LM max duration | ≤ 10 s (500 samples @ 50 Hz); longer = posture change, not a jerk | `MAX_DUR`, `:29` |
| Inter-movement interval | onset-to-onset gap **5–90 s** to belong to the same series | `MIN_GAP`/`MAX_GAP`, `:37-38` |
| PLM series | **≥ 4** consecutive LMs meeting the interval rule | `:48`, `:51` |
| PLMI | total in-series PLMs ÷ recording hours; **clinical threshold ≥ 15/h (adults)** | `:56`, `:58` |

**Algorithm.**
1. Apply `remove_baseline()` internally to each of the three axes so raw or pre-filtered input both work (`:19`).
2. **Vector magnitude** combining all three axes: `vm = sqrt(ax² + ay² + az²)` (`:20`) over the baseline-removed (zero-centered) channels. One activity signal from three channels.
3. **LM detection** — contiguous runs where `vm ≥ threshold` via a `diff`-of-boolean edge finder (`:23-26`); keep runs with `MIN_DUR ≤ (off − on) ≤ MAX_DUR` (`:31-32`). `total_lms` counts every qualifying LM.
4. **PLM series grouping** — walk LM onsets in order; extend the current group while the onset-to-onset gap is within `[MIN_GAP, MAX_GAP]`, else close it; a closed group counts only if it has **≥ 4** members (`:40-52`).
5. **Output** (`:65-68`): `lm_events` (sec), `plm_groups`, `total_lms`, `total_plms`, `plmi`, `total_hours`, and the `vm` trace.

**Subtleties a port must reproduce:**
- `vm` is built from the **already-baseline-removed** channels, so the threshold is an above-baseline amplitude, matching the AASM "above baseline" wording.
- `total_hours = n_samples / fs / 3600` (`:57`) — wall-clock recording length, **not** summed movement time. PLMI denominator decision; do not change without a [decision-log entry](../state/decisions.md).
- Grouping is on **onset-to-onset** gaps, not gaps between offset and next onset.
- A trailing in-progress group is flushed after the loop (`:51`) — the last series is not dropped.
- **Threshold recalibration open item.** `threshold=8` was tuned against the old ADXL335-derived uint8 byte-format scale (0–255, centered ~128). The current input is the MMA8451's physical **mg** output (roughly ±2000 range) — a materially different scale. The default has not been re-validated against real movement data; treat it as a placeholder pending recalibration.

> **Known edge-case to tighten on port.** The grouping guard is `if total_lms >= 2` (`:41`) but a valid series needs ≥ 4 LMs; the `len(current) >= 4` checks (`:48`,`:51`) make the result correct, but the `>= 2` gate is loose. Reference tests below pin the *result*, so the port is free to use the cleaner `>= 4` gate. Tracked under [open forks](../state/decisions.md).

`accel_magnitude(ax, ay, az, window_sec=30, fs=50)` (`:71`) is the same `vm` computation exposed standalone (no thresholding) — feeds the [viewer](viewer.md) as a derived activity trace.

### 3. HRV — RMSSD

`compute_hrv(rr_series, fs=1, window_sec=300)` (`:80`), with helper `_rmssd(arr) = sqrt(mean(diff(arr)²))` (`:76`).

**Intent.** Heart-rate variability is a [UARS](concepts.md) / arousal signal — RMSSD (root mean square of successive RR differences) is the standard short-term HRV metric.

**Algorithm.**
1. The RR series holds the *most recent* RR value at each sample (a sample-and-hold, at the EDF+'s native **1 Hz** RR rate), so first extract **actual beat transitions** at `np.diff(rr) != 0` and read the RR value + timestamp at each change (`:84-86`).
2. **Artifact rejection** — keep only physiologically plausible RR in **300–2000 ms** (i.e. 30–200 bpm); discard the rest as artefacts / dropouts (`:89-91`).
3. `overall = _rmssd(beat_rr)` over all valid beats (`:96`).
4. **Sliding trailing-window RMSSD** — an O(N) two-pointer over a `window_sec` (default 300 s = 5 min) trailing window, emitting a `(t, rmssd)` point per beat with ≥ 2 beats in the window (`:99-107`).
5. Returns `(overall_rmssd, t_hrv[], rmssd_hrv[])`; returns `(nan, [], [])` if < 2 valid beats (`:93-94`).

**Spec invariants:**
- RMSSD is `sqrt(mean(successive_diff²))` of the RR series, in milliseconds.
- Beats are extracted from sample-and-hold transitions, not taken per-sample (otherwise the diffs are mostly zero and RMSSD collapses).
- The 300–2000 ms plausibility gate is part of the definition.
- The trailing window is **5 minutes** and trailing (causal), not centered.

---

## Property / reference tests (the verification loop)

Per [coding standards](../standards/coding.md), the spec is pinned by **property-based + reference tests** that assert on **scoring results**, not byte layouts — so the C++ port is verified against the same suite. Targets:

**`remove_baseline`**
- *Property:* output of a constant input equals the constant (DC removed).
- *Property:* a sharp jerk shorter than the window survives (peak-to-baseline amplitude preserved within tolerance); a slow ramp is suppressed.
- *Property:* idempotent-ish — re-running on already-flat output changes it negligibly.
- *Reference:* fixed-seed accel fixture → byte-exact (current) / value-exact-within-tol (port) baseline-removed output.

**`count_plm`**
- *Reference:* hand-built fixtures with a **known** LM/PLM count — e.g. exactly 4 jerks 10 s apart → 1 series, 4 PLMs; 3 jerks → 0 PLMs (below the ≥4 gate); jerks 100 s apart → LMs but 0 PLMs (gap > 90 s); a 0.3 s blip → 0 LMs (< MIN_DUR); a 12 s plateau → 0 LMs (> MAX_DUR).
- *Property:* `total_plms ≤ total_lms` always; `plmi == total_plms / total_hours`.
- *Reference:* a committed example EDF+ recording (e.g. under `examples/`) → recorded golden `(total_lms, total_plms, plmi)` per accelerometer (regression lock).

**`compute_hrv`**
- *Reference:* a synthetic RR series with a known RMSSD → exact match; all-equal RR → RMSSD 0.
- *Property:* RR values outside 300–2000 ms never affect the output; < 2 valid beats → `nan` (no crash).
- *Property:* trailing window only uses beats within `window_sec` before each emitted point.

---

## Future roadmap (undecided and unbuilt)

These are the **copious** features the project exists to build. All are [open forks](../state/decisions.md) — documented here as scope, not committed implementation. The clinical reasoning behind each lives in [concepts](concepts.md).

- **Apnea / hypopnea detection** — the multi-signal core: airflow cessation + [RIP](concepts.md) belt behaviour distinguishes [obstructive vs central apnea](concepts.md) (paradoxical belt movement with no flow = obstructive; flat belts + no flow = central). Produces AHI and per-event annotations into `events.json`.
- **Arousal detection** — fragments-of-sleep scoring; pairs with HRV for the [UARS](concepts.md) picture (HR spikes without large O₂ desat).
- **Airflow signal processing** — [`README.md`](../../README.md) expects "fairly standard noise filtering" once a real SDP800 `Flow` sample is captured; not yet specified ([open fork](../state/decisions.md)).
- **EEG** — not yet captured by the [hardware](hardware.md); sleep-stage scoring (Wake/N1/N2/N3/REM) is a future fork awaiting an EEG channel (BDF+ 24-bit path noted in [data formats](data-formats.md)).
- **Snore VOTE / MFCC ML** — the end-game: classify the anatomical snore site (**V**elum, **O**ropharynx, **T**ongue base, **E**piglottis) from the [FLAC audio sidecar](data-formats.md) via MFCC/Mel features + a fast real-time ML model trained on the **MPSSC** (Munich-Passau Snore Sound Corpus). The audio is processed into a **precomputed spectrogram array stored in Zarr**, which the [viewer](viewer.md) renders (wavesurfer.js) without re-deriving. This is the PyTorch / scikit-learn path that most justifies Python at this stage.

---

## Long-term: Python-vs-C++ watch-item

**This stage is Python today by deliberate prototyping choice, and that is a documented risk, not a settled forever-decision** ([open fork](../state/decisions.md), raised by Dmitry).

- **For the prototype:** Python is right — scipy/numpy/scikit/PyTorch are the de-facto standard for "finding salient features in signals", and the [ML training](#future-roadmap-undecided-and-unbuilt) path effectively requires it.
- **The tension:** C++ wins on performance and on **medical-software compliance** ([IEC 62304](../standards/coding.md) aspiration), and modern C++23 can make scientific code terse enough to stay readable. The processing **may migrate Python → C++** later.
- **Packaging cost:** Python is the heaviest leg of the [air-gapped packaging](../planning/packaging.md) case (a pinned conda env or a vendored wheelhouse, per-OS). Migrating to C++ would shed that leg.
- **Why the test discipline matters here:** because the algorithm is pinned by [result-level reference tests](#property--reference-tests-the-verification-loop) (not byte layouts), a future C++ reimplementation is verified against the *same* suite — the spec survives the language change. That is the whole point of treating the Python as an executable specification.

This is documented as a **risk to watch, not a change to make now**.

---

See also: [architecture](architecture.md) · [data formats](data-formats.md) · [concepts](concepts.md) · [hardware](hardware.md) · [viewer](viewer.md) · [coding standards](../standards/coding.md) · [packaging](../planning/packaging.md) · [decisions / open forks](../state/decisions.md).
