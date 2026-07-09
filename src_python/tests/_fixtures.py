"""Shared synthetic-signal helper for the windowed-metrics tests. Not a test
module itself (no test_ prefix), just a builder shared across test files.
"""

import numpy as np


def make_jerk_signal(duration_s, fs, onsets_s, jerk_duration_s=1.0, jerk_amplitude=20.0):
    """A flat 3-axis accel signal (mg) with a single-axis jerk at each onset.

    The baseline is 0 everywhere except during a jerk, so `remove_baseline`'s
    rolling-median subtraction leaves each brief jerk intact (the window is
    far longer than any jerk) while contributing no drift of its own — this
    fixture only needs to exercise AASM duration/gap/series logic, not
    baseline removal itself.
    """
    n = int(round(duration_s * fs))
    ax = np.zeros(n, dtype=float)
    for onset_s in onsets_s:
        start = int(round(onset_s * fs))
        end = start + int(round(jerk_duration_s * fs))
        ax[start:end] += jerk_amplitude
    ay = np.zeros(n, dtype=float)
    az = np.zeros(n, dtype=float)
    return ax, ay, az
