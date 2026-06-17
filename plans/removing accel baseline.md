# Plan: Accelerometer Baseline Removal + AASM PLM Counting for PLMD Analysis

## Context
The binary log records accelerometer (x/y/z) + RR data at 10 Hz. Two capabilities:
1. **Baseline removal** (already implemented): rolling median subtraction to strip step changes while preserving jerks.
2. **PLM counting** (this task): count periodic limb movements per AASM scoring rules and report total count + PLMI (jerks/hour).

---

## New Function: `count_plm(data, threshold=8, fs=10)`

### AASM PLM Scoring Rules (AASM Scoring Manual v2.6)
These are the published criteria the function must implement:

| Criterion | Rule |
|---|---|
| **LM amplitude** | Signal deviation ≥ threshold above baseline |
| **LM min duration** | ≥ 0.5 s (5 samples @ 10 Hz) |
| **LM max duration** | ≤ 10 s (100 samples @ 10 Hz) — longer events are posture changes, not jerks |
| **Inter-movement interval** | Onset-to-onset gap of **5–90 s** for two LMs to belong to the same series |
| **PLM series** | ≥ 4 consecutive LMs meeting the interval rule |
| **PLMI** | Total PLMs (in series) ÷ recording hours. Clinical threshold: ≥ 15/hour (adults) |

### Algorithm Steps

1. **Baseline removal** — apply `remove_baseline()` internally (re-centered at 128) so the function works on either raw or pre-filtered data.
2. **Vector magnitude** — `vm = sqrt((ax-128)² + (ay-128)² + (az-128)²)` per sample. This combines all three channels into a single activity signal.
3. **LM detection** — find contiguous runs where `vm >= threshold`. Record onset index for each run. Discard runs shorter than 5 samples (< 0.5 s) or longer than 100 samples (> 10 s).
4. **PLM series grouping** — walk through LM onsets in order; group consecutive pairs whose onset-to-onset gap is 5–90 s. A group of ≥ 4 qualifies as a PLM series; all its LMs are counted as PLMs.
5. **Output**:
   - Recording duration in hours
   - Total LMs detected (all events passing duration filter)
   - Total PLMs (LMs in qualifying series)
   - PLMI with reference to the ≥ 15/h diagnostic threshold

### CLI
Add `-c` / `--count` flag and `--threshold` option to `main()`:
- `--count`: run PLM counting on the loaded data, print results, then exit
- `--threshold`: amplitude threshold in accel units (default: 8)

Can be combined with `-f` to specify an input file (use pre-filtered `.bin` for best results).

## Files to Modify
**`ESP32-S3-heart/read_log.py`** — add `count_plm()` function and two CLI args (`-c`/`--count`, `--threshold`).

No new imports needed (numpy already imported).

## Verification
```
# Count PLMs from filtered binary (recommended):
python read_log.py -f biometric_filtered.bin -c

# Count PLMs from raw binary (baseline removal applied internally):
python read_log.py -f biometric.bin -c

# Adjust threshold:
python read_log.py -f biometric_filtered.bin -c --threshold 12

# Expected output:
# Recording duration: X.XX hours
# LMs detected: N
# PLMs (in series of ≥4, 5–90 s apart): M
# PLMI: M.M /hour  [AASM diagnostic threshold: ≥15/hour for adults]
```
