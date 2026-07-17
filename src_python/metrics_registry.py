"""The windowed-metrics registry: how a new windowed metric gets added.

Each `MetricSpec` wraps an existing pure function from `signal_processing.py`
(no AASM/HRV logic is duplicated here) and declares what it needs: which Zarr
arrays to read for a given `channel`, how much boundary padding its algorithm
needs (see the plan doc §3 for why PLM needs padding and RMSSD doesn't), and
the minimum window length below which a result would be meaningless.

Adding a new windowed metric means adding one more `MetricSpec` to `METRICS`
— no endpoint code changes (`metrics_service.py` is generic over the
registry).
"""

from dataclasses import dataclass
from typing import Callable

from signal_processing import count_plm, combine_bilateral_vm
from metrics_windowing import filter_lm_events, filter_plm_group_members, windowed_hours


@dataclass(frozen=True)
class MetricSpec:
    metric: str
    channel_options: tuple
    zarr_arrays: Callable[[str], tuple]
    compute: Callable[[dict, float, dict], dict]
    params_schema: dict
    context_before_s: float
    context_after_s: float
    min_window_s: float
    result_fields: tuple


def _plmi_channels(channel):
    return {
        'accel_mag':          ('accel_x', 'accel_y', 'accel_z'),
        'accel1_mag':         ('accel1_x', 'accel1_y', 'accel1_z'),
        'accel_combined_mag': ('accel_x', 'accel_y', 'accel_z', 'accel1_x', 'accel1_y', 'accel1_z'),
    }[channel]


def _plmi_compute(arrays, fs, params):
    threshold = params.get('threshold', 8.0)
    # 0 means "gate off" over the wire, matching read_log.py's CLI; the DSP
    # spells that None (0 would reject everything, not nothing).
    max_threshold = params.get('max_threshold', 500.0) or None
    tilt = params.get('tilt_threshold_deg', 10.0) or None
    gpc_args = {'max_threshold': max_threshold, 'tilt_threshold_deg': tilt}

    if 'accel1_x' not in arrays:
        return count_plm(arrays['accel_x'], arrays['accel_y'], arrays['accel_z'],
                         threshold=threshold, fs=fs, **gpc_args)
    if 'accel_x' not in arrays:
        return count_plm(arrays['accel1_x'], arrays['accel1_y'], arrays['accel1_z'],
                         threshold=threshold, fs=fs, **gpc_args)
    r0 = count_plm(arrays['accel_x'], arrays['accel_y'], arrays['accel_z'],
                   threshold=threshold, fs=fs, **gpc_args)
    r1 = count_plm(arrays['accel1_x'], arrays['accel1_y'], arrays['accel1_z'],
                   threshold=threshold, fs=fs, **gpc_args)
    return combine_bilateral_vm(r0['vm'], r1['vm'], threshold=threshold, fs=fs,
                                max_threshold=max_threshold,
                                gpc0=r0['gpc'], gpc1=r1['gpc'])


PLMI_SPEC = MetricSpec(
    metric='plmi',
    channel_options=('accel_mag', 'accel1_mag', 'accel_combined_mag'),
    zarr_arrays=_plmi_channels,
    compute=_plmi_compute,
    params_schema={
        'threshold':         {'type': 'number', 'default': 8.0,   'min': 0.0},
        'max_threshold':     {'type': 'number', 'default': 500.0, 'min': 0.0},
        'tilt_threshold_deg': {'type': 'number', 'default': 10.0, 'min': 0.0, 'max': 180.0},
    },
    # 120 s of padding already covers GPC detection's needs: the tilt at a sample
    # reads the gravity baseline half a window (15 s) either side, and each
    # baseline point is itself a 30 s-window median — so the tilt at the window
    # edge depends on raw samples up to 30 s outside it.
    context_before_s=120.0, context_after_s=120.0, min_window_s=15.0,
    result_fields=('plmi', 'total_lms', 'total_plms', 'total_hours'),
)

METRICS = {'plmi': PLMI_SPEC}


def is_window_too_short(spec, start_s, end_s):
    return (end_s - start_s) < spec.min_window_s


def compute_windowed_metric(spec, arrays, fs, start_s, end_s, padded_start_s, params):
    """Run `spec.compute` over the padded arrays, then shift its event times
    from padded-slice-relative back to recording-relative seconds (the
    algorithm has no idea it was handed a slice — its outputs are indices
    into whatever array it was given) before filtering to the requested
    window per the plan's §3 windowed-PLMI definition.
    """
    raw = spec.compute(arrays, fs, params)

    lm_events = [(on + padded_start_s, off + padded_start_s) for on, off in raw.get('lm_events', [])]
    plm_groups = [
        [(on + padded_start_s, off + padded_start_s) for on, off in group]
        for group in raw.get('plm_groups', [])
    ]

    windowed_lms = filter_lm_events(lm_events, start_s, end_s)
    windowed_plm_members = filter_plm_group_members(plm_groups, start_s, end_s)
    hours = windowed_hours(start_s, end_s)
    total_plms = len(windowed_plm_members)

    return {
        'plmi': (total_plms / hours) if hours > 0 else 0.0,
        'total_lms': len(windowed_lms),
        'total_plms': total_plms,
        'total_hours': hours,
    }
