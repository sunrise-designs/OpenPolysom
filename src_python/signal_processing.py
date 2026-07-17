import numpy as np
from scipy.ndimage import median_filter

# A rolling-median baseline weaker than this cannot be gravity (1 g ~= 1000 mg),
# so the sensor's orientation is not recoverable from it. Inputs that carry no
# gravity — already-baseline-removed traces, synthetic fixtures — land here and
# GPC detection degrades to a no-op rather than inventing rotations. See
# `gravity_tilt`.
MIN_GRAVITY_MG = 100.0


def gravity_baseline(channels, window_sec=30, fs=50):
    """The rolling median of each channel — the slow component `remove_baseline`
    subtracts.

    For *raw* accelerometer axes this is an estimate of the gravity vector, and
    hence of the sensor's orientation: the median rejects brief jerks, so what
    survives is posture, not movement. Exposed separately from `remove_baseline`
    because GPC detection needs the baseline itself, not the residual.
    """
    window = int(window_sec * fs)
    # reflect mode avoids one-sided lag artifacts at the edges
    return [median_filter(np.asarray(ch, dtype=float), size=window, mode='reflect')
            for ch in channels]


def remove_baseline(channels, window_sec=30, fs=50):
    """Subtract a rolling median from each channel (jerk-preserving baseline removal).

    channels: iterable of 1-D array-likes, already in physical units.
    Returns a list of baseline-removed float arrays, same order/scale as input.
    """
    # Materialise before walking `channels` twice (once to build the baseline,
    # once to subtract it): the parameter is documented as any *iterable*, and a
    # generator would be exhausted by the first pass, silently returning [].
    channels = [np.asarray(ch, dtype=float) for ch in channels]
    return [ch - base
            for ch, base in zip(channels, gravity_baseline(channels, window_sec=window_sec, fs=fs))]


def _tilt_from_baseline(baseline, window_sec, fs):
    """`gravity_tilt`, given an already-computed baseline. Kept separate so a
    caller that needs both the residual and the tilt pays for the (costly)
    rolling median once rather than twice.
    """
    g = np.stack(baseline)
    half = max(1, int(window_sec * fs / 2))
    n = g.shape[1]
    i = np.arange(n)
    before = g[:, np.clip(i - half, 0, n - 1)]
    after  = g[:, np.clip(i + half, 0, n - 1)]

    norm_before = np.linalg.norm(before, axis=0)
    norm_after  = np.linalg.norm(after,  axis=0)
    known = (norm_before >= MIN_GRAVITY_MG) & (norm_after >= MIN_GRAVITY_MG)

    cos = np.divide((before * after).sum(axis=0), norm_before * norm_after,
                    out=np.ones(n), where=known)
    return np.where(known, np.degrees(np.arccos(np.clip(cos, -1.0, 1.0))), 0.0)


def gravity_tilt(ax, ay, az, window_sec=30, fs=50):
    """Per-sample angle (degrees) between the gravity vector `window_sec`/2 before
    and `window_sec`/2 after each sample — how far the sensor rotated across it.

    This is the Gross-Position-Change discriminator. A limb jerk does not rotate
    the sensor: the median-filtered gravity vector either side of it is the same,
    so the angle reads ~0°. A gross position change (a roll, a turn) leaves the
    sensor pointing somewhere new, so the before/after gravity vectors diverge and
    the angle is large. Amplitude alone cannot tell the two apart — a hard kick and
    a roll can both swing hundreds of mg — but rotation can.

    The comparison is centred (half a window either side) so the returned angle
    peaks *over* the rotation rather than lagging it, and so the elevated span
    brackets the movement that caused it — which is what lets `gpc_mask` also
    catch the spurious LMs a GPC drags in on either side of itself.

    Returns 0° wherever either gravity vector is weaker than `MIN_GRAVITY_MG`:
    orientation is undefined there, and an undefined rotation must not read as a
    large one (arccos of a 0/0 quotient would otherwise fabricate 90°).
    """
    return _tilt_from_baseline(
        gravity_baseline([ax, ay, az], window_sec=window_sec, fs=fs), window_sec, fs)


def gpc_mask(ax, ay, az, tilt_threshold_deg=10.0, window_sec=30, fs=50):
    """Per-sample boolean mask — True inside a Gross Position Change.

    True wherever the sensor's orientation relative to gravity is changing by more
    than `tilt_threshold_deg` across a `window_sec` span. An LM overlapping any
    True sample is a posture change (or debris thrown off by one) and is not
    scored as a limb movement.
    """
    return gravity_tilt(ax, ay, az, window_sec=window_sec, fs=fs) >= tilt_threshold_deg


def _score_vm(vm, threshold=8, fs=50, max_threshold=None, gpc=None):
    """Detect LMs in a vector-magnitude trace and group them into PLM series
    per AASM rules. Factored out of `count_plm` so `combine_bilateral_vm` can
    apply the identical grouping logic to a combined two-leg trace — the AASM
    invariants (duration/gap/series-length) then live in exactly one place.

    `max_threshold` (peak vm ceiling) and `gpc` (a `gpc_mask` boolean trace)
    reject Gross Position Changes. Both are optional and independent: the ceiling
    catches a violent transient that happened not to rotate the sensor (a device
    knock, taking the unit off), the mask catches a roll of any amplitude. A
    rejected run is reported under `gpc_events` rather than dropped silently.
    """
    vm = np.asarray(vm, dtype=float)
    gpc = None if gpc is None else np.asarray(gpc, dtype=bool)

    # Find contiguous runs above threshold (AASM: 0.5–10 s duration)
    above = (vm >= threshold).astype(int)
    diff  = np.diff(above, prepend=0, append=0)
    raw_onsets  = np.where(diff ==  1)[0]
    raw_offsets = np.where(diff == -1)[0]

    MIN_DUR = int(0.5 * fs)   #   5 samples
    MAX_DUR = int(10.0 * fs)  # 100 samples

    lm_pairs, gpc_pairs = [], []
    for on, off in zip(raw_onsets, raw_offsets):
        if not (MIN_DUR <= (off - on) <= MAX_DUR):
            continue
        rotated = gpc is not None and bool(gpc[on:off].any())
        too_big = max_threshold is not None and float(vm[on:off].max()) > max_threshold
        (gpc_pairs if (rotated or too_big) else lm_pairs).append((on, off))
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
    gpc_events  = [(on / fs, off / fs) for on, off in gpc_pairs]

    return {'lm_events': lm_events, 'plm_groups': plm_groups,
            'total_lms': total_lms, 'total_plms': total_plms,
            'plmi': plmi, 'total_hours': total_hours,
            'gpc_events': gpc_events, 'total_gpcs': len(gpc_events),
            'vm': list(vm)}


def count_plm(ax, ay, az, threshold=8, fs=50, max_threshold=None, tilt_threshold_deg=10.0,
              window_sec=30):
    """Score one accelerometer's three raw axes for AASM LMs/PLMs, excluding
    Gross Position Changes.

    `threshold` is the LM amplitude floor; `max_threshold` the ceiling above which
    a run is a GPC rather than a jerk; `tilt_threshold_deg` the rotation-vs-gravity
    a run may not exceed (see `gravity_tilt`).

    Either GPC gate is disabled by passing None (not 0 — a 0° tilt threshold would
    make every sample a GPC, and a 0 mg ceiling every movement one).

    **The defaults are deliberately asymmetric.** The tilt gate is on by default
    because a degree is a degree whatever the axes are scaled in. The ceiling is
    off by default because it is denominated in the input's own amplitude units,
    and this function does not know what those are — `threshold=8` still carries
    the retired uint8 scale while real callers now pass mg, so a baked-in mg
    default here would be an assertion about units the signature cannot make.
    Callers that know their units (`read_log.py`, `metrics_registry.py`) pass 500
    mg explicitly. See wiki/knowledge/signal-processing.md § 2b.

    GPC rejection reads the gravity vector out of the *raw* axes, so it only
    engages on gravity-bearing input. Hand this already-baseline-removed axes and
    the tilt gate self-disables (`MIN_GRAVITY_MG`) — the amplitude gates still
    apply.
    """
    # Baseline removal is applied internally so raw or pre-filtered data both
    # work. The baseline is kept rather than discarded: it *is* the gravity
    # vector the tilt gate needs, and the rolling median that produces it is by
    # far the most expensive step here — computing it once serves both.
    baseline = gravity_baseline([ax, ay, az], window_sec=window_sec, fs=fs)
    fax, fay, faz = [np.asarray(ch, dtype=float) - base
                     for ch, base in zip((ax, ay, az), baseline)]
    vm = np.sqrt(fax**2 + fay**2 + faz**2)

    gpc = None
    if tilt_threshold_deg is not None:
        gpc = _tilt_from_baseline(baseline, window_sec, fs) >= tilt_threshold_deg

    result = _score_vm(vm, threshold=threshold, fs=fs,
                       max_threshold=max_threshold, gpc=gpc)
    result['gpc'] = gpc

    print(f"Recording duration : {result['total_hours']:.2f} hours")
    print(f"LMs detected       : {result['total_lms']}")
    print(f"GPCs excluded      : {result['total_gpcs']}")
    print(f"PLMs (series >=4, 5-90 s apart): {result['total_plms']}")
    print(f"PLMI               : {result['plmi']:.1f} /hour  [AASM threshold >=15/hour for adults]")

    return result


def combine_bilateral_vm(vm0, vm1, threshold=8, fs=50, max_threshold=None,
                         gpc0=None, gpc1=None):
    """Combine two legs' vector-magnitude traces into one bilateral LM/PLM score.

    Per common bilateral PLM scoring practice (combining left/right leg EMG
    into a single channel before scoring), take the envelope — elementwise
    max — of both legs' vm traces, then re-run the same AASM LM/PLM detection
    over that combined trace. A movement on *either* leg counts once, and
    overlapping bilateral movements are not double-counted.

    `gpc0`/`gpc1` are each leg's `count_plm()` `gpc` mask. They are **unioned**,
    not intersected: the combined trace is an elementwise max, so a rotation on
    either leg contaminates it, and a whole-body position change need only rotate
    one sensor to have thrown the other limb around too. Truncation to the shorter
    leg happens here, alongside the vm truncation, so a mask can never slip out of
    alignment with the trace it gates.
    """
    vm0 = np.asarray(vm0, dtype=float)
    vm1 = np.asarray(vm1, dtype=float)
    n = min(len(vm0), len(vm1))
    combined_vm = np.maximum(vm0[:n], vm1[:n])

    masks = [np.asarray(g, dtype=bool)[:n] for g in (gpc0, gpc1) if g is not None]
    gpc = np.logical_or.reduce(masks) if masks else None

    return _score_vm(combined_vm, threshold=threshold, fs=fs,
                     max_threshold=max_threshold, gpc=gpc)


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
