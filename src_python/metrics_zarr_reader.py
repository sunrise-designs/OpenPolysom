"""The imperative shell for the windowed-metrics service: locates a
recording's Zarr store + meta.json on disk and slices the requested arrays to
a padded sample range. This is the one module a cloud deployment swaps out
(e.g. for an fsspec/S3-backed store) — `metrics_registry.py` and
`metrics_windowing.py` never touch storage directly.
"""

import json
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import zarr


@dataclass(frozen=True)
class RecordingLocation:
    recording_id: str
    zarr_path: Path
    meta_path: Path


class RecordingNotFoundError(Exception):
    pass


def locate_recording(recordings_root, recording_id):
    """Mirror the stem convention `export_zarr.py:save_zarr_json` already
    uses: `<stem>.zarr` + `<stem>_meta.json` beside each other."""
    stem = Path(recordings_root) / recording_id
    zarr_path = stem.with_suffix('.zarr')
    meta_path = Path(str(stem) + '_meta.json')
    if not zarr_path.exists() or not meta_path.exists():
        raise RecordingNotFoundError(recording_id)
    return RecordingLocation(recording_id=recording_id, zarr_path=zarr_path, meta_path=meta_path)


def load_meta(location):
    return json.loads(location.meta_path.read_text(encoding='utf-8'))


def sample_rate(meta):
    return float(meta['recording']['sample_rate_hz'])


def recording_duration_s(meta):
    return float(meta['recording']['duration_s'])


def read_window(location, array_names, padded_start_s, padded_end_s, fs):
    """Open the Zarr group and slice each named array to the padded sample
    range. Every array here shares the accelerometer time axis (`t`), so one
    `fs` covers all of them (true for the PLM-channel arrays this service
    reads today; a future metric spanning arrays at different native rates
    would need per-array rates, not a blocking concern yet)."""
    root = zarr.open_group(str(location.zarr_path), mode='r')
    start_idx = max(0, int(round(padded_start_s * fs)))
    end_idx = int(round(padded_end_s * fs))

    arrays = {}
    for name in array_names:
        if name not in root:
            continue
        data = np.asarray(root[name][start_idx:end_idx])
        if data.size > 0:
            arrays[name] = data
    return arrays
