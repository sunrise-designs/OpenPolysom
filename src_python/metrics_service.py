"""FastAPI service computing windowed clinical metrics on demand.

Runs as its own process (see `serve_metrics.py`), separate from
`export_zarr.py:serve_and_open`'s static-file dev server — the split matters
because in the cloud case the static viewer bundle (Netlify) and this compute
service live on different hosts. See `wiki/state/decisions.md` for the O10
distinction (this is not the deferred raw-sample slicing server).

`RECORDINGS_ROOT` is the one runtime-configurable seam: where a recording_id
resolves to a `.zarr` store + `_meta.json` on disk. `metrics_zarr_reader.py`
is the module a cloud deployment would swap for fsspec/S3-backed storage.
"""

import os
from datetime import datetime, timezone
from pathlib import Path
from typing import Annotated

from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field, field_validator

from export_zarr import DEFAULT_OUT_DIR, _git_provenance
from metrics_registry import METRICS, compute_windowed_metric, is_window_too_short
from metrics_windowing import pad_window
from metrics_zarr_reader import (
    RecordingNotFoundError,
    load_meta,
    locate_recording,
    read_window,
    recording_duration_s,
    sample_rate,
)

RECORDINGS_ROOT = Path(os.environ.get('PROTOSOM_RECORDINGS_ROOT', DEFAULT_OUT_DIR))

app = FastAPI(title='ProtoSom windowed metrics service', version='0.1.0')
app.add_middleware(CORSMiddleware, allow_origins=['*'], allow_methods=['*'], allow_headers=['*'])


class MetricWindow(BaseModel):
    start_s: Annotated[float, Field(allow_inf_nan=False)]
    end_s: Annotated[float, Field(allow_inf_nan=False)]

    @field_validator('end_s')
    @classmethod
    def _end_after_start(cls, end_s, info):
        start_s = info.data.get('start_s')
        if start_s is not None and end_s <= start_s:
            raise ValueError('end_s must be greater than start_s')
        return end_s


class MetricRequestItem(BaseModel):
    metric: str
    channel: str
    params: dict[str, float] = Field(default_factory=dict)


class MetricsRequest(BaseModel):
    window: MetricWindow
    metrics: list[MetricRequestItem]


@app.post('/v1/recordings/{recording_id}/metrics')
def compute_metrics(recording_id: str, body: MetricsRequest):
    try:
        location = locate_recording(RECORDINGS_ROOT, recording_id)
    except RecordingNotFoundError:
        raise HTTPException(status_code=404, detail=f'unknown recording_id: {recording_id}')

    meta = load_meta(location)
    fs = sample_rate(meta)
    duration_s = recording_duration_s(meta)
    start_s, end_s = body.window.start_s, body.window.end_s

    results = []
    for item in body.metrics:
        spec = METRICS.get(item.metric)
        if spec is None:
            raise HTTPException(status_code=400, detail=f'unknown metric: {item.metric}')
        if item.channel not in spec.channel_options:
            raise HTTPException(
                status_code=400,
                detail=f'unknown channel {item.channel!r} for metric {item.metric!r}',
            )

        if is_window_too_short(spec, start_s, end_s):
            results.append({
                'metric': item.metric, 'channel': item.channel, 'status': 'error',
                'error': {
                    'code': 'window_too_short',
                    'message': (f'window is {end_s - start_s:.1f}s; {item.metric} '
                                f'requires >= {spec.min_window_s:.1f}s'),
                },
            })
            continue

        padded = pad_window(start_s, end_s, spec.context_before_s, spec.context_after_s, duration_s)
        array_names = spec.zarr_arrays(item.channel)
        arrays = read_window(location, array_names, padded.padded_start_s, padded.padded_end_s, fs)

        if not all(name in arrays for name in array_names):
            results.append({
                'metric': item.metric, 'channel': item.channel, 'status': 'error',
                'error': {
                    'code': 'channel_unavailable',
                    'message': f'{item.channel} is not available for this recording',
                },
            })
            continue

        params = {**{k: v['default'] for k, v in spec.params_schema.items()}, **item.params}
        value = compute_windowed_metric(spec, arrays, fs, start_s, end_s, padded.padded_start_s, params)

        results.append({
            'metric': item.metric, 'channel': item.channel, 'status': 'ok',
            'params': {**params, 'fs': fs},
            'window_used': {
                'start_s': start_s, 'end_s': end_s,
                'context_before_s': spec.context_before_s, 'context_after_s': spec.context_after_s,
                'padded_start_s': padded.padded_start_s, 'padded_end_s': padded.padded_end_s,
                'clipped_to_recording_bounds': padded.clipped_to_recording_bounds,
            },
            'value': value,
        })

    return {
        'recording_id': recording_id,
        'requested_window': {'start_s': start_s, 'end_s': end_s},
        'results': results,
        'provenance': {
            'git': _git_provenance(),
            'service': 'metrics_service',
            'computed_at': datetime.now(timezone.utc).isoformat(),
        },
    }
