"""Pure windowing helpers for the windowed clinical-metrics service.

No numpy/zarr/I-O here — this is the part of the windowed-metrics pipeline
that is cheap to test exhaustively and easy to port to Rust later. See
`metrics_registry.py` for how these compose with `signal_processing.py`'s
AASM scoring, and the plan doc for why padding is needed at all: a metric
like PLM/PLMI scored on a naively-sliced `[start_s, end_s]` window would
mis-score events near the boundary (the baseline median filter needs
context, and a PLM series can span the window edge).
"""

from dataclasses import dataclass


@dataclass(frozen=True)
class PaddedWindow:
    start_s: float
    end_s: float
    padded_start_s: float
    padded_end_s: float
    context_before_s: float
    context_after_s: float
    clipped_to_recording_bounds: bool


def pad_window(start_s, end_s, context_before_s, context_after_s, recording_duration_s):
    """Expand [start_s, end_s] by the requested context on each side, clipped
    to the recording's own bounds [0, recording_duration_s].

    The *requested* window (start_s/end_s) is always preserved verbatim in the
    result — only the padded bounds are clipped — so callers can still define
    "windowed" metrics (e.g. PLMI's hours denominator) off the user's actual
    selection, never the padded span.
    """
    raw_padded_start = start_s - context_before_s
    raw_padded_end = end_s + context_after_s
    padded_start = max(0.0, raw_padded_start)
    padded_end = min(recording_duration_s, raw_padded_end)
    clipped = padded_start != raw_padded_start or padded_end != raw_padded_end
    return PaddedWindow(
        start_s=start_s, end_s=end_s,
        padded_start_s=padded_start, padded_end_s=padded_end,
        context_before_s=context_before_s, context_after_s=context_after_s,
        clipped_to_recording_bounds=clipped,
    )


def filter_lm_events(lm_events, start_s, end_s):
    """Keep only LM events (on, off) second-tuples whose *onset* falls inside
    [start_s, end_s) — onset-based inclusion, matching how AASM series
    membership is decided (see `signal_processing.py::_score_vm`)."""
    return [(on, off) for on, off in lm_events if start_s <= on < end_s]


def filter_plm_group_members(plm_groups, start_s, end_s):
    """Flatten PLM group members (each group a list of (on, off) tuples) down
    to just the member LMs whose onset falls inside [start_s, end_s).

    Group *qualification* (>=4 members, 5-90s onset gaps) must already have
    been decided over the full padded context before calling this — this
    function only decides which of a qualifying group's members actually
    happened inside the requested window, giving correct partial credit to a
    series that straddles the window boundary.
    """
    members = []
    for group in plm_groups:
        members.extend((on, off) for on, off in group if start_s <= on < end_s)
    return members


def windowed_hours(start_s, end_s):
    """The requested window's own duration in hours — never the padded
    duration. This is what a windowed rate metric (e.g. PLMI) divides by."""
    return (end_s - start_s) / 3600.0
