---
title: Signal Processing (Python)
domain: knowledge
status: living
updated: 2026-07-17
summary: The Python processing stage — baseline removal, AASM PLM/LM, Gross Position Change rejection, bilateral-combined PLM, HRV/RMSSD — documented as the executable spec, with the future apnea / EEG / snore roadmap and the Python-vs-C++ watch-item.
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

`count_plm(ax, ay, az, threshold=8, fs=50, max_threshold=None, tilt_threshold_deg=10.0, window_sec=30)`.
The trailing three arguments are [§ 2b](#2b-gross-position-change-gpc-rejection)'s; the rest of this
section describes the AASM core. Periodic Limb Movement scoring per the **AASM Scoring Manual v2.6** (rules transcribed in [`plans/removing accel baseline.md`](../../plans/removing%20accel%20baseline.md)). Runs once per accelerometer — the ESP32-C6 logger has two (Accel0, Accel1) — scored independently, **and** combined into one bilateral score (below); all three are exported and plotted, see [current state](../state/current.md) and [viewer](viewer.md).

The threshold/duration/gap/grouping logic (`:23-58` of the pre-refactor code) is
factored into a shared `_score_vm(vm, threshold, fs)` helper that takes an
already-computed vector-magnitude trace and returns the same `lm_events` /
`plm_groups` / `plmi` shape. `count_plm` computes `vm` from one accelerometer's
three axes and calls it; `combine_bilateral_vm` (below) computes a *combined*
`vm` from both accelerometers and calls the identical helper — so the AASM
invariants (duration/gap/series-length) are defined in exactly one place
regardless of which vm trace is being scored.

**AASM rules implemented (the spec — never silently change these):**

| Criterion | Rule | In code |
|---|---|---|
| LM amplitude | vector magnitude ≥ `threshold` above baseline | `:23` |
| LM min duration | ≥ 0.5 s (25 samples @ 50 Hz) | `MIN_DUR`, `:28` |
| LM max duration | ≤ 10 s (500 samples @ 50 Hz); longer = posture change, not a jerk | `MAX_DUR`, `:29` |
| Inter-movement interval | onset-to-onset gap **5–90 s** to belong to the same series | `MIN_GAP`/`MAX_GAP`, `:37-38` |
| PLM series | **≥ 4** consecutive LMs meeting the interval rule | `:48`, `:51` |
| PLMI | total in-series PLMs ÷ recording hours; **clinical threshold ≥ 15/h (adults)** | `:56`, `:58` |

> **`window_sec` now actually reaches the baseline (changed 2026-07-17).** `count_plm`
> used to call `remove_baseline(..., fs=fs)` *without* `window_sec`, so PLM detection
> silently used the 30 s default no matter what `--window` said — and `meta.json`
> recorded the argparse value, which was therefore a lie whenever it wasn't 30. The
> [zarr schema spec § 6.5](../planning/zarr-schema-spec.md) flags this and recommends
> exactly this fix. `count_plm` now takes `window_sec` and threads it through, and
> `read_log.py` passes `--window`. **Only affects runs with `--window` ≠ 30**; the
> default path is unchanged, and the recorded provenance is now true.

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

### 2b. Gross Position Change (GPC) rejection

`gravity_baseline` · `gravity_tilt` · `gpc_mask`, plus the `max_threshold` /
`tilt_threshold_deg` arguments on `count_plm`.

**Intent.** A night's accelerometry contains two kinds of movement, and only one
is an LM. A **limb movement** is a jerk: the limb accelerates and returns, and the
sensor ends up pointing where it started. A **Gross Position Change** — rolling
over, turning — moves the whole body, swings a far larger amplitude, and leaves
the sensor pointing somewhere new. Before this gate, GPCs both scored as LMs
themselves and **drafted** spurious neighbours: the baseline step throws off extra
threshold crossings either side of the roll, so one position change could
manufacture a handful of "LMs".

The AASM `MAX_DUR` rule (≤10 s) already catches the slowest position changes. It
does not catch a fast one, which is why these two gates exist.

**The two gates** (independent; either alone rejects a run, and both are optional):

| Gate | Rule | Catches |
|---|---|---|
| **Amplitude ceiling** | peak `vm` > `max_threshold` ⇒ not an LM | A violent transient that never rotated the sensor — a device knock, the unit coming off |
| **Rotation** | orientation vs gravity changes > `tilt_threshold_deg` across the run ⇒ not an LM | A roll of any amplitude, plus the LMs it drafts |

**Algorithm (rotation gate).**
1. `gravity_baseline` — the rolling median *kept* rather than subtracted. For raw
   axes the median rejects jerks, so what survives is gravity: the sensor's
   orientation.
2. `gravity_tilt` — per sample, the angle between the gravity vector `window_sec`/2
   **before** and **after** it: `θ = arccos( (g₋ · g₊) / (|g₋| |g₊|) )`. A jerk leaves
   both vectors equal (θ≈0°); a roll makes them diverge (θ large). **Amplitude cannot
   separate the two — a hard kick and a roll both swing hundreds of mg — rotation can.**
3. `gpc_mask` — `θ ≥ tilt_threshold_deg`. An LM overlapping any True sample is dropped.
4. Rejected runs are reported as `gpc_events` / `total_gpcs`, not dropped silently.

**Spec invariants (what a port must hold):**
- The tilt comparison is **centred** (half a window either side), not trailing. This
  is load-bearing twice over: the angle peaks *over* the rotation instead of lagging
  it, and the elevated span **brackets** the movement — which is what suppresses the
  drafted LMs on either side. A trailing comparison would leave them scored.
- **Rotation is undefined without gravity.** Where either gravity vector is weaker
  than `MIN_GRAVITY_MG` (100 mg — 1 g ≈ 1000 mg), tilt is **0°, never a rotation**.
  An unguarded `arccos` of a 0/0 quotient fabricates 90° and would mark every sample
  a GPC. This is what makes the gate degrade to a no-op on already-baseline-removed
  or synthetic input rather than destroying the score.
- Gating is **subtractive only**: `lm_events` after ⊆ `lm_events` before, and
  `total_lms + total_gpcs` == the ungated `total_lms`.
- `combine_bilateral_vm` **unions** the two legs' masks, never intersects them: the
  combined trace is an elementwise max, so a rotation on either leg contaminates it,
  and a whole-body position change need only rotate one sensor to have thrown the
  other limb around too. Masks truncate to the shorter leg alongside the traces.
- The gates are disabled with `None`, **never 0** — a 0° tilt threshold marks every
  sample a GPC, and a 0 mg ceiling every movement one. `read_log.py`'s CLI accepts 0
  as "off" and maps it to `None` at the edge.

> **Threshold provenance — read this before trusting the defaults.**
> `tilt_threshold_deg=10.0` and `max_threshold=500.0` (mg) were tuned against **one**
> recording, `biometric_2026-07-16_23-00-00.zarr` (2 h, 50 Hz, MMA8451 mg). In it the
> tilt distribution is starkly bimodal — p50 0.02°, p90 0.09°, versus 13–124° at the
> four position changes — so **any** threshold in ~5–15° separates them identically;
> 10° is not a knife-edge. The ceiling is the weaker of the two: real LMs peaked at
> ≤264 mg and GPC/artefact runs at ≥641 mg in that recording, but a genuinely violent
> kick could exceed 500 mg, and unlike the rotation gate that would be a false
> rejection. Both are **scale-dependent placeholders**, in the same position as
> `threshold=8` (see §2) — they mean nothing across a units change and are not
> validated against a corpus. See [decisions § S11](../state/decisions.md).

### 3. Bilateral combined PLM score

`combine_bilateral_vm(vm0, vm1, threshold=8, fs=50, max_threshold=None, gpc0=None, gpc1=None)`. Standard PSG practice
scores leg movements per-leg **and** on one combined bilateral channel; a
movement on *either* leg counts once toward the combined index, without
double-counting movements both legs make together. This is the headline
number the [viewer](viewer.md) surfaces (its charts show all three: Accel0
alone, Accel1 alone, and the combined trace).

**Algorithm.** Takes each leg's already-computed `vm` trace (`count_plm`'s
`vm` output — the baseline-removed, per-leg vector magnitude, *not* raw
axes), truncates both to the shorter length (the two legs' trimmed sample
counts should already match since `read_log.py` trims both accelerometers
identically, but a length mismatch would otherwise silently misalign), then
takes the **elementwise max** — the envelope of whichever leg is more active
at each instant — and runs the identical AASM `_score_vm` grouping (§2) over
that combined trace.

**Spec invariants:**
- `combined_vm[i] = max(vm0[i], vm1[i])` — no leg's movement is diluted by
  averaging with a quiescent other leg.
- The combined trace is re-scored from scratch with the same
  duration/gap/series-length rules as a single leg — it is **not** a union or
  merge of the two legs' already-detected LM event lists (a merge would skip
  re-applying the onset-to-onset gap rule to the merged series and under-count
  bilateral PLM series).
- `read_log.py` treats the combined result as `stats`/the headline score
  (`meta.json`'s top-level `stats.plmi`/`total_lms`/`total_plms`); each leg's
  own `count_plm()` result is carried alongside as `stats.legs.accel0` /
  `.accel1` (per-leg PLMI breakdown, no vm trace) — see
  [data formats](data-formats.md) and [viewer](viewer.md).

### 4. HRV — RMSSD

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

**GPC rejection** (`src_python/tests/test_gpc_rejection.py` — built)
- *Property:* a still sensor reads 0° tilt; a jerk of **any** amplitude that does not
  rotate the sensor reads ~0°; a synthetic roll of θ° recovers θ° within tolerance.
- *Property:* gravity-free input (zeros, or |g| < `MIN_GRAVITY_MG`) reads **0°, not 90°**
  — the regression guard on the 0/0 `arccos`. Scoring a gravity-free fixture gated and
  ungated gives the identical result, which is why the pre-existing windowed-metrics
  fixtures are untouched by this feature.
- *Property:* gating is subtractive — `total_lms` gated ≤ ungated, `lm_events` gated ⊆
  ungated, and `total_lms + total_gpcs` == ungated `total_lms`.
- *Reference:* a roll → 0 LMs and ≥1 GPC with `max_threshold=None` (isolating the
  rotation gate, so the ceiling cannot take credit); jerks 2 s either side of a roll →
  0 LMs (the drafting case); a jerk 250 s away → still scored (the GPC span must not
  swallow the recording).
- *Reference:* a 900 mg transient that never rotates → 1 LM ungated, 0 LMs + 1 GPC with
  `max_threshold=500` (isolating the ceiling — the case rotation cannot see).
- *Reference (bilateral):* leg 0 rolls where leg 1 shows a clean jerk → combined scores
  **0** LMs; passing only leg 1's mask scores ≥1, proving the **union** rejected it.
- **Fixture note:** a GPC fixture **must** include a transient burst on top of the
  orientation step. A rolling median passes a step edge through untouched, so an
  idealised instant roll leaves a residual of ~16 mg — far under any threshold — and
  would never be mistaken for an LM in the first place. What makes a real GPC
  masquerade as a giant LM is the acceleration needed to rotate the limb. A
  step-only fixture tests nothing.

**`combine_bilateral_vm`**
- *Property:* `combined_vm == elementwise max(vm0, vm1)`, so a movement scored on one leg alone still shows up in the combined trace at full amplitude.
- *Reference:* leg 0 has a qualifying jerk where leg 1 is flat (and vice versa) → the combined score's `total_lms` counts both; two legs jerking at the exact same instant → the combined score counts it **once**, not twice.
- *Property:* `combined.total_plms ≥ max(accel0.total_plms, accel1.total_plms)` — combining can only reveal a bilateral series that neither leg alone met the ≥4-count gate for, never hide one.

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
