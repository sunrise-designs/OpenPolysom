import pytest

from _fixtures import make_jerk_signal
from metrics_registry import PLMI_SPEC, compute_windowed_metric, is_window_too_short
from metrics_windowing import pad_window
from signal_processing import count_plm

FS = 50.0
DURATION_S = 600.0
THRESHOLD = 8.0


def _windowed(ax, ay, az, start_s, end_s, context_before_s, context_after_s):
    """Mirror what metrics_service.py does per-request: pad, slice, compute —
    kept local to the test so these registry-level tests don't depend on the
    Zarr I/O layer (metrics_zarr_reader.py) at all.
    """
    padded = pad_window(start_s, end_s, context_before_s, context_after_s, DURATION_S)
    i0 = int(round(padded.padded_start_s * FS))
    i1 = int(round(padded.padded_end_s * FS))
    arrays = {'accel_x': ax[i0:i1], 'accel_y': ay[i0:i1], 'accel_z': az[i0:i1]}
    return compute_windowed_metric(PLMI_SPEC, arrays, FS, start_s, end_s, padded.padded_start_s,
                                    {'threshold': THRESHOLD})


def test_reference_matches_direct_count_plm_when_series_fully_inside_window():
    onsets = [250.0, 260.0, 270.0, 280.0]  # one qualifying 4-member series, 10s gaps
    ax, ay, az = make_jerk_signal(DURATION_S, FS, onsets)
    start_s, end_s = 200.0, 340.0  # ample margin on both sides of the series

    windowed = _windowed(ax, ay, az, start_s, end_s, PLMI_SPEC.context_before_s, PLMI_SPEC.context_after_s)

    i0, i1 = int(start_s * FS), int(end_s * FS)
    direct = count_plm(ax[i0:i1], ay[i0:i1], az[i0:i1], threshold=THRESHOLD, fs=FS)

    assert windowed['total_lms'] == direct['total_lms'] == 4
    assert windowed['total_plms'] == direct['total_plms'] == 4
    assert windowed['plmi'] == pytest.approx(direct['plmi'])


def test_windowed_whole_recording_matches_full_night_count_plm():
    """Consistency property: requesting the entire recording as the window
    must reproduce exactly what a plain full-night count_plm() call gives —
    the windowing/filtering machinery must be a no-op when the window happens
    to be the whole night.
    """
    onsets = [100.0, 110.0, 120.0, 130.0, 300.0, 310.0, 320.0, 330.0]
    ax, ay, az = make_jerk_signal(DURATION_S, FS, onsets)

    full = count_plm(ax, ay, az, threshold=THRESHOLD, fs=FS)
    windowed = _windowed(ax, ay, az, 0.0, DURATION_S, PLMI_SPEC.context_before_s, PLMI_SPEC.context_after_s)

    assert windowed['total_lms'] == full['total_lms']
    assert windowed['total_plms'] == full['total_plms']
    assert windowed['plmi'] == pytest.approx(full['plmi'])


def test_boundary_lm_scored_with_full_context_not_truncated():
    """A qualifying 4-member series (250, 260, 270, 280) with the window
    boundary landing mid-jerk on the 3rd member: end_s=270.2 leaves only 0.2s
    of that 1s jerk inside the raw [start_s, end_s) slice — below AASM's
    MIN_DUR=0.5s. A naive (unpadded) computation would drop it; the padded
    computation must see the whole jerk via after-window context and detect
    it correctly, while a 4th member (onset 280) stays excluded by onset.
    """
    onsets = [250.0, 260.0, 270.0, 280.0]
    ax, ay, az = make_jerk_signal(DURATION_S, FS, onsets, jerk_duration_s=1.0)
    start_s, end_s = 200.0, 270.2

    windowed = _windowed(ax, ay, az, start_s, end_s, PLMI_SPEC.context_before_s, PLMI_SPEC.context_after_s)
    assert windowed['total_lms'] == 3
    assert windowed['total_plms'] == 3  # all 3 are members of the (padded-visible) qualifying series

    # Contrast: naive hard slice at exactly [start_s, end_s), no padding —
    # the 3rd jerk is truncated to 0.2s and silently dropped.
    i0, i1 = int(start_s * FS), int(end_s * FS)
    naive = count_plm(ax[i0:i1], ay[i0:i1], az[i0:i1], threshold=THRESHOLD, fs=FS)
    assert naive['total_lms'] == 2


def test_series_starting_beyond_fixed_padding_is_undercounted():
    """Documented residual limitation (plan doc §3): the fixed 120s padding
    is a practical bound, not a guarantee. A series whose earlier members lie
    more than context_before_s before start_s can fail the AASM >=4 gate in
    the padded computation even though a wider-context computation would
    recognize it. This test pins that as *expected* current behavior — it
    should go red (not silently start passing) if padding is ever widened or
    made dynamic without a deliberate decision.
    """
    onsets = [50.0, 100.0, 150.0, 200.0, 250.0, 300.0]  # 6-member series, 50s gaps
    ax, ay, az = make_jerk_signal(DURATION_S, FS, onsets)
    start_s, end_s = 300.0, 340.0

    narrow = _windowed(ax, ay, az, start_s, end_s, PLMI_SPEC.context_before_s, PLMI_SPEC.context_after_s)
    # Wide context sees all 6 members, so the series clearly qualifies (>=4).
    wide = _windowed(ax, ay, az, start_s, end_s, context_before_s=400.0, context_after_s=PLMI_SPEC.context_after_s)

    assert wide['total_plms'] == 1     # the one member (onset 300.0) inside [start_s, end_s)
    assert narrow['total_plms'] == 0   # only 3 of 6 members visible within the fixed 120s
                                        # padding (200, 250, 300) — below the AASM >=4 gate,
                                        # so the whole series, in-window member included, is
                                        # silently dropped by the fixed-padding default.


def test_window_too_short_flag():
    assert is_window_too_short(PLMI_SPEC, 100.0, 105.0) is True    # 5s < 15s min
    assert is_window_too_short(PLMI_SPEC, 100.0, 120.0) is False   # 20s >= 15s min
