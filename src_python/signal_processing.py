import numpy as np
from scipy.ndimage import median_filter


def remove_baseline(channels, window_sec=30, fs=50):
    """Subtract a rolling median from each channel (jerk-preserving baseline removal).

    channels: iterable of 1-D array-likes, already in physical units.
    Returns a list of baseline-removed float arrays, same order/scale as input.
    """
    window = int(window_sec * fs)
    # reflect mode avoids one-sided lag artifacts at the edges
    return [np.asarray(ch, dtype=float) - median_filter(np.asarray(ch, dtype=float), size=window, mode='reflect')
            for ch in channels]


def _score_vm(vm, threshold=8, fs=50):
    """Detect LMs in a vector-magnitude trace and group them into PLM series
    per AASM rules. Factored out of `count_plm` so `combine_bilateral_vm` can
    apply the identical grouping logic to a combined two-leg trace — the AASM
    invariants (duration/gap/series-length) then live in exactly one place.
    """
    vm = np.asarray(vm, dtype=float)

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
    total_hours = len(vm) / fs / 3600
    plmi        = total_plms / total_hours if total_hours > 0 else 0.0

    return {'lm_events': lm_events, 'plm_groups': plm_groups,
            'total_lms': total_lms, 'total_plms': total_plms,
            'plmi': plmi, 'total_hours': total_hours,
            'vm': list(vm)}


def count_plm(ax, ay, az, threshold=8, fs=50):
    # Apply baseline removal internally so raw or pre-filtered data both work
    fax, fay, faz = remove_baseline([ax, ay, az], fs=fs)
    vm = np.sqrt(fax**2 + fay**2 + faz**2)
    result = _score_vm(vm, threshold=threshold, fs=fs)

    print(f"Recording duration : {result['total_hours']:.2f} hours")
    print(f"LMs detected       : {result['total_lms']}")
    print(f"PLMs (series ≥4, 5–90 s apart): {result['total_plms']}")
    print(f"PLMI               : {result['plmi']:.1f} /hour  [AASM threshold ≥15/hour for adults]")

    return result


def combine_bilateral_vm(vm0, vm1, threshold=8, fs=50):
    """Combine two legs' vector-magnitude traces into one bilateral LM/PLM score.

    Per common bilateral PLM scoring practice (combining left/right leg EMG
    into a single channel before scoring), take the envelope — elementwise
    max — of both legs' vm traces, then re-run the same AASM LM/PLM detection
    over that combined trace. A movement on *either* leg counts once, and
    overlapping bilateral movements are not double-counted.
    """
    vm0 = np.asarray(vm0, dtype=float)
    vm1 = np.asarray(vm1, dtype=float)
    n = min(len(vm0), len(vm1))
    combined_vm = np.maximum(vm0[:n], vm1[:n])
    return _score_vm(combined_vm, threshold=threshold, fs=fs)


def accel_magnitude(ax, ay, az, window_sec=30, fs=50):
    fax, fay, faz = remove_baseline([ax, ay, az], window_sec=window_sec, fs=fs)
    return list(np.sqrt(fax**2 + fay**2 + faz**2))


def _rmssd(arr):
    return float(np.sqrt(np.mean(np.diff(arr) ** 2)))


def compute_hrv(rr_series, fs=1, window_sec=300):
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
