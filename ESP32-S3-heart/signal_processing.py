import struct
import numpy as np
from scipy.ndimage import median_filter


def remove_baseline(data, window_sec=30, fs=10):
    records = [(data[i], data[i+1], data[i+2], struct.unpack_from('<H', data, i+3)[0])
               for i in range(0, len(data) - 4, 5)]

    channels = [np.array([r[c] for r in records], dtype=float) for c in range(3)]
    rr_vals  = [r[3] for r in records]

    window = int(window_sec * fs)
    # reflect mode avoids one-sided lag artifacts at the edges
    filtered = [ch - median_filter(ch, size=window, mode='reflect') + 128.0
                for ch in channels]

    out = bytearray()
    for i, rr in enumerate(rr_vals):
        ax = int(np.clip(round(filtered[0][i]), 0, 255))
        ay = int(np.clip(round(filtered[1][i]), 0, 255))
        az = int(np.clip(round(filtered[2][i]), 0, 255))
        out += bytes([ax, ay, az]) + struct.pack('<H', rr)
    return bytes(out)


def count_plm(data, threshold=8, fs=10):
    # Apply baseline removal internally so raw or pre-filtered data both work
    filtered = remove_baseline(data, fs=fs)
    records = [(filtered[i], filtered[i+1], filtered[i+2],
                struct.unpack_from('<H', filtered, i+3)[0])
               for i in range(0, len(filtered) - 4, 5)]

    ax = np.array([r[0] for r in records], dtype=float) - 128
    ay = np.array([r[1] for r in records], dtype=float) - 128
    az = np.array([r[2] for r in records], dtype=float) - 128
    vm = np.sqrt(ax**2 + ay**2 + az**2)

    # Find contiguous runs above threshold (AASM: 0.5–10 s duration)
    above = (vm >= threshold).astype(int)
    diff  = np.diff(above, prepend=0, append=0)
    raw_onsets  = np.where(diff ==  1)[0]
    raw_offsets = np.where(diff == -1)[0]

    MIN_DUR = int(0.5 * fs)   #   5 samples
    MAX_DUR = int(10.0 * fs)  # 100 samples

    lm_pairs = [(on, off) for on, off in zip(raw_onsets, raw_offsets)
                if MIN_DUR <= (off - on) <= MAX_DUR]
    total_lms = len(lm_pairs)

    # Group into PLM series: consecutive LMs with onset-to-onset gap 5–90 s
    # A series requires ≥4 LMs (AASM Scoring Manual)
    MIN_GAP = int(5  * fs)   #  50 samples
    MAX_GAP = int(90 * fs)   # 900 samples

    plm_group_indices = []
    if total_lms >= 2:
        current = [0]
        for i in range(1, len(lm_pairs)):
            gap = lm_pairs[i][0] - lm_pairs[i - 1][0]
            if MIN_GAP <= gap <= MAX_GAP:
                current.append(i)
            else:
                if len(current) >= 4:
                    plm_group_indices.append(current)
                current = [i]
        if len(current) >= 4:
            plm_group_indices.append(current)

    lm_events   = [(on / fs, off / fs) for on, off in lm_pairs]
    plm_groups  = [[lm_events[i] for i in grp] for grp in plm_group_indices]
    total_plms  = sum(len(g) for g in plm_groups)
    total_hours = len(records) / fs / 3600
    plmi        = total_plms / total_hours if total_hours > 0 else 0.0

    print(f"Recording duration : {total_hours:.2f} hours")
    print(f"LMs detected       : {total_lms}")
    print(f"PLMs (series ≥4, 5–90 s apart): {total_plms}")
    print(f"PLMI               : {plmi:.1f} /hour  [AASM threshold ≥15/hour for adults]")

    return {'lm_events': lm_events, 'plm_groups': plm_groups,
            'total_lms': total_lms, 'total_plms': total_plms,
            'plmi': plmi, 'total_hours': total_hours}


def _rmssd(arr):
    return float(np.sqrt(np.mean(np.diff(arr) ** 2)))


def compute_hrv(rr_series, fs=10, window_sec=300):
    rr = np.array(rr_series, dtype=float)

    # Each sample holds the most recent RR value; extract actual beat transitions
    changes = np.where(np.diff(rr) != 0)[0] + 1
    beat_rr = rr[changes]
    beat_t  = changes / fs  # seconds

    # Discard physiologically implausible values (artefacts / missing data)
    valid   = (beat_rr >= 300) & (beat_rr <= 2000)
    beat_rr = beat_rr[valid]
    beat_t  = beat_t[valid]

    if len(beat_rr) < 2:
        return float('nan'), np.array([]), np.array([])

    overall = _rmssd(beat_rr)

    # Sliding trailing window RMSSD — O(N) two-pointer
    t_hrv, rmssd_hrv = [], []
    left = 0
    for right in range(1, len(beat_t)):
        while beat_t[right] - beat_t[left] > window_sec:
            left += 1
        seg = beat_rr[left:right + 1]
        if len(seg) >= 2:
            t_hrv.append(beat_t[right])
            rmssd_hrv.append(_rmssd(seg))

    return overall, np.array(t_hrv), np.array(rmssd_hrv)
