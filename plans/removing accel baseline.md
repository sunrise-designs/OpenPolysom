# Plan: Accelerometer Baseline Removal for PLMD Analysis

## Context
The binary log records accelerometer (x/y/z) + RR data at 10 Hz. For PLMD detection, we need to see transient leg jerks while suppressing slow baseline drift and sudden step changes (e.g., body repositioning). Rolling median filter subtraction is the standard technique in actigraphy/PLMD research: the long-window median captures the slowly-varying baseline without being pulled by spikes, so subtracting it leaves only the transient content intact.

## Algorithm: Rolling Median Subtraction
**Why this over a high-pass Butterworth filter:** A Butterworth HPF rings at step discontinuities, creating false spike-like artifacts — exactly what we want to avoid. Median filters are edge-preserving and are widely used in PLMD actigraphy literature (e.g., Cole-Kripke, Sadeh, and subsequent PLMD scoring algorithms all use median-based activity normalization).

- Compute a rolling median of each accel channel with a configurable window (default **30 s = 300 samples** at 10 Hz — long enough to span full PLMD cycles of 20–40 s, short enough to track postural drift).
- Subtract the rolling median from the raw signal → zero-mean, spike-preserving residual.
- Re-center at **128** (midpoint of uint8 range) so the output can be repacked into the same 5-byte record format.
- Clamp to **[0, 255]** and write as uint8. RR values are passed through unchanged.

Uses `scipy.ndimage.median_filter` (mode=`'reflect'` for edge handling, which avoids the one-sided lag a causal filter would introduce).

## Files to Modify
**`ESP32-S3-heart/read_log.py`** — single file, three changes:

1. **Add import** at top:
   ```python
   import numpy as np
   from scipy.ndimage import median_filter
   ```

2. **Add function** `remove_baseline(data, window_sec=30, fs=10)`:
   - Parse all records (same logic as `plot()`).
   - Build numpy arrays for accel_x, accel_y, accel_z.
   - Apply `median_filter(channel, size=window_sec * fs)` per channel.
   - Subtract median, add 128, clip to [0, 255], cast to uint8.
   - Repack as 5-byte records: `bytes([ax, ay, az]) + struct.pack('<H', rr)`.
   - Return the new binary blob.

3. **Add CLI argument** in `main()`:
   ```
   -b / --baseline   Remove baseline from .bin file and write filtered output.
                     Takes the input path (from -f/--file or default OUT).
                     Writes to <stem>_filtered.bin.
   --window          Median window in seconds (default: 30).
   ```
   When `--baseline` is passed: load the file, call `remove_baseline()`, write `_filtered.bin`, print confirmation, exit (no plotting). Can be combined with `-f` to specify the input file.

## Verification
```
# Filter an existing binary log:
python read_log.py -f biometric.bin -b

# Plot the filtered output to compare visually:
python read_log.py -f biometric_filtered.bin

# Check that step changes are gone and spikes are preserved by comparing
# the two HTML plots side-by-side.
```
