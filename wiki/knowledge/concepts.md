---
title: PSG & Pipeline Concepts (Glossary)
domain: knowledge
status: living
updated: 2026-08-20
summary: A concise glossary of the polysomnography, sleep-disorder, signal and data-tech terms used across the ProtoSom wiki and codebase.
---

# PSG & Pipeline Concepts

A working glossary for ProtoSom. It defines the sleep-medicine terms behind the
signals we capture and the data-tech terms behind how we store and show them.
For *what we do* with these, see [signal processing](signal-processing.md),
[data formats](data-formats.md), [hardware](hardware.md) and the
[viewer](viewer.md); for *why* a thing is the way it is, see
[decisions](../state/decisions.md).

---

## Polysomnography (PSG)

**Polysomnography (PSG)** — a multi-signal sleep study: recording several
physiological channels simultaneously through a night's sleep so that breathing,
cardiac, movement and (here) acoustic events can be scored together. ProtoSom is
an open-source proof-of-concept PSG; no single channel diagnoses anything — it
takes a *confluence* of signals to steer a finding (`README.md:62`). It is
**not a medical device** (`README.md:134`).

The signals ProtoSom records ([hardware](hardware.md), `README.md:8`):
Respiratory Inductance Plethysmography (RIP, two belts), nasal airflow, heart
rate + RR intervals, wrist accelerometry, and audio for snore analysis.

---

## Sleep disorders & respiratory events

These are the *findings* the [Python processing](signal-processing.md) aims to
surface from the captured signals. Plain-language sketches per `README.md:51-60`.

- **Apnea** — a near-total cessation of airflow (conventionally ≥10 s in adults).
  Classified by what the RIP belts show:
  - **Obstructive apnea (OSA)** — *no airflow but the chest/abdomen belts still
    move*, often in opposite phase ("paradoxical breathing") as the body heaves
    against a collapsed upper airway (tongue/soft palate). Blood O₂ drops.
  - **Central apnea (CSA)** — *no airflow and no belt movement*: the drive to
    breathe is absent ("the brain forgot to breathe"). Blood O₂ drops.
  - **Mixed apnea** — a single event that begins central (no effort) and becomes
    obstructive (effort returns against a blocked airway), or vice versa.
- **Hypopnea** — a *partial* reduction in airflow (commonly ≥30% drop) sustained
  ≥10 s, paired with an O₂ desaturation and/or an arousal. The graded cousin of
  an apnea.
- **RERA (Respiratory Effort-Related Arousal)** — increasing breathing effort
  that ends in a cortical arousal but does **not** meet apnea/hypopnea
  thresholds. Visible as effort climbing on the RIP belts with airflow only
  mildly reduced.
- **UARS (Upper Airway Resistance Syndrome)** — repeated RERAs fragmenting sleep
  with *little or no O₂ desaturation*; the body becomes hypersensitive to
  breathing effort, often signalled by **heart-rate spikes** rather than
  desaturation. Can score "healthy" on a conventional apnea scale yet still be
  serious (`README.md:57`).
- **PLMD (Periodic Limb Movement Disorder)** — repetitive limb jerks in sleep
  that fragment it. Detected here from wrist accelerometry by AASM PLM scoring
  (see [PLM / AASM](#aasm-scoring-epochs--indices) and
  [`plans/removing accel baseline.md`](../../plans/removing%20accel%20baseline.md)).

---

## Captured signals

### Respiration

- **RIP (Respiratory Inductance Plethysmography)** — two inductive belts
  (Thoracic + Abdomen) whose inductance changes as the cross-sectional area of
  the ribcage/abdomen changes with breathing. ProtoSom reads them via an
  **LDC1612** inductance-to-frequency sensor at 50 Hz; the EDF+ header declares
  nanohenries but the channel actually carries pre-divided raw counts
  ([hardware](hardware.md)). Thoracic−Abdomen phase is the
  central/obstructive discriminator above.
- **Nasal airflow** — airflow rate measured by a differential-pressure sensor
  (**SDP800-125Pa**, ±125 Pa, 50 Hz). The "is air actually moving" channel that
  apnea/hypopnea scoring keys off. Expected to need standard noise filtering
  (`README.md:74-76`).
- **QDC (Qualitative Diagnostic Calibration)** — a Sackner (1989) method for
  combining the two RIP belts into a calibrated estimate of tidal volume / airflow
  surrogate. Used here alongside **airPLS** for RIP baseline removal
  (`README.md:66-68`; see [signal processing](signal-processing.md)).
- **airPLS (Adaptive Iteratively Reweighted Penalized Least Squares)** — an
  iterative baseline-fitting algorithm (originally for spectra) that estimates and
  subtracts a slow drifting baseline while preserving the breathing waveform on
  top. ProtoSom's planned RIP baseline-removal step (`README.md:68`).

### Cardiac

- **Heart rate (HR)** — beats per minute. **Not a captured channel**: the
  ESP32-C6 has no `HR` channel. Heart rate is something to *derive* from the ECG
  trace, not a signal read off a sensor.
- **RR interval** — the time (ms) between successive R-peaks of consecutive
  heartbeats; the beat-to-beat timing series. A 2.5 Hz channel on the ESP32-C6,
  but currently a **dead channel logging zeros** — the device has no R-peak
  detection ([hardware](hardware.md)). Note: *RR interval* (cardiac) is distinct
  from *RR = respiratory rate* in other PSG literature — in ProtoSom "RR" means
  the cardiac interval.
- **HRV (Heart Rate Variability)** — variability of the RR-interval series; a
  proxy for autonomic state. Sleep-disordered breathing perturbs it, and HR/HRV
  spikes are a key UARS tell.
- **RMSSD (Root Mean Square of Successive Differences)** — the standard
  time-domain HRV metric: the RMS of the differences between successive RR
  intervals. Computed by `compute_hrv` in
  [`src_python/signal_processing.py`](../../src_python/signal_processing.py).
- **ECG (electrocardiogram)** — the raw cardiac electrical trace; an AD8232 ADC
  channel (`ECG`, 100 Hz) and the device's **only** cardiac source. R-peaks (and
  hence RR intervals, and hence HRV) would be derived from it — that derivation is
  not yet built on-device or host-side.

### Movement

- **Accelerometry** — 3-axis acceleration (Accel0X/Y/Z, Accel1X/Y/Z) from two
  MMA8451 accelerometers on the ESP32-C6, reported in **mg at 50 Hz**. Two of them
  gives the bilateral (per-leg plus combined) scoring standard PSG practice wants.
  Used to detect limb movement for PLMD; baseline drift (posture changes) is
  stripped by rolling-median subtraction before PLM detection
  ([`plans/removing accel baseline.md`](../../plans/removing%20accel%20baseline.md)).
- **Vector magnitude** — `sqrt(ax² + ay² + az²)` per sample over the
  **baseline-removed** (zero-centred) axes: the three accel axes collapsed into one
  activity signal for limb-movement detection. (The `−128` centring seen in older
  notes belonged to the retired uint8 byte-log format, not to the current mg scale.)

### Acoustic (snoring)

- **Snoring** — sound produced by vibration of soft tissue in the upper airway
  during sleep; ProtoSom records audio (high-quality USB mic, `README.md:89`) as a
  separate channel for classification, not just detection.
- **VOTE** — a clinical scheme naming the *anatomical site* of upper-airway
  vibration/collapse: **V**elum, **O**ropharynx (lateral walls), **T**ongue base,
  **E**piglottis. The snore-classification end-goal is to label the VOTE site from
  the sound (`README.md:70-72`).
- **MFCC (Mel-Frequency Cepstral Coefficients) / Mel** — audio features on the
  **Mel scale** (a perceptual, roughly-log frequency scale). MFCCs are a compact
  spectral summary widely used in audio ML; planned as the snore feature set
  feeding a model trained on the **MPSSC** (Munich-Passau Snore Sound Corpus)
  (`README.md:72`).
- **Spectrogram** — a time × frequency × intensity image of a signal (here, the
  audio). [Python processing](signal-processing.md) precomputes the spectrogram
  array into the [working store](data-formats.md) so the
  [viewer](viewer.md) (wavesurfer.js audio pane) can render it without recomputing.

---

## AASM scoring, epochs & indices

- **AASM** — the American Academy of Sleep Medicine, whose *Scoring Manual* (we
  reference v2.6) defines the standard rules for scoring sleep events. ProtoSom's
  PLM counting implements its published criteria
  ([`plans/removing accel baseline.md`](../../plans/removing%20accel%20baseline.md)).
- **Epoch** — the fixed time window sleep is scored in, conventionally **30 s**.
  Each epoch gets a sleep stage; events are tallied across epochs.
- **Hypnogram** — the staged timeline of a night's sleep (the sequence of sleep
  stages across all epochs), the classic step-plot summary of sleep architecture.
- **LM (Limb Movement)** — a single qualifying limb movement: amplitude ≥ a
  threshold above baseline, duration **0.5–10 s** (25–500 samples @ 50 Hz; longer
  = posture change, not a jerk).
- **PLM (Periodic Limb Movement)** — LMs occurring in a **series of ≥4** with
  onset-to-onset gaps of **5–90 s**. Counted by `count_plm` per AASM rules
  ([`plans/removing accel baseline.md`](../../plans/removing%20accel%20baseline.md)).
- **GPC (Gross Position Change)** — a whole-body posture change (rolling over,
  turning) as opposed to a limb jerk. Swings a far larger amplitude than an LM
  and, unlike an LM, leaves the accelerometer **pointing somewhere new** — that
  rotation relative to gravity, not the amplitude, is what reliably tells the two
  apart. GPCs are excluded from LM/PLM scoring; see
  [signal processing § 2b](signal-processing.md) and
  [decisions § S11](../state/decisions.md). ProtoSom-specific terminology (the AASM
  manual speaks only of movements "longer than 10 s" being posture change), coined
  by Dmitry.
- **Index (events/hour)** — events normalised to recording duration:
  - **AHI (Apnea-Hypopnea Index)** — (apneas + hypopneas) ÷ sleep hours; the
    headline severity number for sleep-disordered breathing.
  - **RDI (Respiratory Disturbance Index)** — like AHI but *including RERAs*, so
    it catches UARS-type fragmentation that the AHI misses.
  - **PLMI (PLM Index)** — PLMs ÷ recording hours; adult diagnostic threshold
    **≥15/hour**.

---

## Data & technology terms

These describe how ProtoSom stores and serves the data across its three-language
pipeline (**C++ ingest → Python processing → the TS web app**); see
[data formats](data-formats.md) and [architecture](architecture.md).

- **EDF / EDF+** — *European Data Format* (and the `+` revision adding
  annotations/events), the de-facto open standard for biosignal recordings. EDF+
  is ProtoSom's **raw anchor** for biosignals — what the device emits, immutable —
  and is read via **edflib**. EDFBrowser opens it directly, which is why it's also
  the **clinical export** format (`README.md:13`).
- **BDF+** — BioSemi Data Format, an EDF+ variant using **24-bit** samples
  (vs EDF's 16-bit). Reserved for future higher-resolution EEG.
- **FLAC** — *Free Lossless Audio Codec*. The **raw anchor** for the audio
  channel: a lossless, time-anchored sidecar to the EDF+ biosignals.
- **Zarr** — a chunked, compressed, language-neutral array store. It is **the
  Zarr boundary** of the pipeline: [C++ ingest](architecture.md) writes the raw
  Zarr, [Python processing](signal-processing.md) reads it and writes the derived
  Zarr + JSON metadata, and the [TS web app](viewer.md) *reads* it (never writes).
  The **working store** is one Zarr group per recording — each dense signal its
  own array — plus JSON sidecars (`events.json`, `meta.json`). We pin **Zarr v2 +
  Blosc(zstd, shuffle)** with no Python-only filters so all three languages
  interoperate (see [decisions](../state/decisions.md)).
- **Chunking** — splitting a Zarr array into fixed-size blocks (here, along the
  time axis) so a reader can fetch just the slice it needs without loading the
  whole array. The basis of the windowed read the [viewer](viewer.md) uses.
- **Decimation** — reducing a signal's sample count to what a target needs (e.g.
  screen-resolution for the visible window). "The data the chart needs" is the
  visible window at screen resolution, **not** the whole recording.
- **LTTB (Largest-Triangle-Three-Buckets)** — a downsampling algorithm that picks
  the points which best preserve a line's *visual shape*, so a decimated chart
  still looks right. Used by the [viewer](viewer.md)'s charting (ECharts).
- **The slicing server** — a thin TS server that reads the Zarr and serves the
  browser screen-resolution windows over an HTTP windowed API
  (`/window`, `/spectrogram`, `/meta`, `/events`) once recordings outgrow a
  browser-direct read. Same reading code, just moved server-side; it decimates
  on-the-fly (precomputed pyramids later, without changing the viewer). See
  [architecture](architecture.md) and [viewer](viewer.md).
- **Provenance / content-hash** — the **raw anchor** is content-hashed and never
  modified; every regenerable **derived layer** product is stamped with provenance
  in `meta.json` so any result can be traced back to its inputs (the IEC 62304
  reproducibility/auditability aspiration, `README.md:83-85`;
  [standards/coding](../standards/coding.md)).

---

## See also

- [architecture](architecture.md) — the four-stage pipeline and three-layer data model.
- [data formats](data-formats.md) — EDF+/FLAC raw anchor, Zarr working store, EDF+ clinical export.
- [signal processing](signal-processing.md) — baseline removal, PLM, HRV, snore, airflow.
- [hardware](hardware.md) — sensors, channels, sample rates, dtypes.
- [viewer](viewer.md) — the TS web app, charts, spectrogram pane.
- [decisions](../state/decisions.md) — settled choices and open forks.
