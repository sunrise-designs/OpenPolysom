import json
import shutil

import numpy as np
import pytest
import zarr
from fastapi.testclient import TestClient

import metrics_service
from _fixtures import make_jerk_signal

FS = 50.0
DURATION_S = 600.0


def _write_fixture_recording(root, recording_id, onsets, with_accel1=False):
    ax, ay, az = make_jerk_signal(DURATION_S, FS, onsets)
    zarr_path = root / f'{recording_id}.zarr'
    if zarr_path.exists():
        shutil.rmtree(zarr_path)
    group = zarr.open_group(str(zarr_path), mode='w', zarr_format=2)
    for name, data in (('accel_x', ax), ('accel_y', ay), ('accel_z', az)):
        arr = group.create_array(name, shape=data.shape, dtype=np.float32, chunks=data.shape)
        arr[...] = data.astype(np.float32)
    if with_accel1:
        for name in ('accel1_x', 'accel1_y', 'accel1_z'):
            arr = group.create_array(name, shape=ax.shape, dtype=np.float32, chunks=ax.shape)
            arr[...] = np.zeros_like(ax, dtype=np.float32)

    meta = {
        'recording_id': recording_id,
        'recording': {'duration_s': DURATION_S, 'sample_rate_hz': FS},
    }
    (root / f'{recording_id}_meta.json').write_text(json.dumps(meta))
    return recording_id


@pytest.fixture
def client(tmp_path, monkeypatch):
    _write_fixture_recording(tmp_path, 'fixture-rec', onsets=[250.0, 260.0, 270.0, 280.0])
    monkeypatch.setattr(metrics_service, 'RECORDINGS_ROOT', tmp_path)
    return TestClient(metrics_service.app)


def test_ok_response_shape(client):
    resp = client.post('/v1/recordings/fixture-rec/metrics', json={
        'window': {'start_s': 200.0, 'end_s': 340.0},
        'metrics': [{'metric': 'plmi', 'channel': 'accel_mag', 'params': {'threshold': 8.0}}],
    })
    assert resp.status_code == 200
    body = resp.json()
    assert body['recording_id'] == 'fixture-rec'
    assert body['requested_window'] == {'start_s': 200.0, 'end_s': 340.0}
    assert 'provenance' in body and 'git' in body['provenance']

    result = body['results'][0]
    assert result['metric'] == 'plmi'
    assert result['channel'] == 'accel_mag'
    assert result['status'] == 'ok'
    assert result['value']['total_lms'] == 4
    assert result['value']['total_plms'] == 4
    assert result['window_used']['context_before_s'] == 120.0
    assert result['window_used']['padded_start_s'] == 80.0


def test_unknown_recording_id_returns_404(client):
    resp = client.post('/v1/recordings/does-not-exist/metrics', json={
        'window': {'start_s': 0.0, 'end_s': 20.0}, 'metrics': [],
    })
    assert resp.status_code == 404


def test_malformed_window_returns_422(client):
    resp = client.post('/v1/recordings/fixture-rec/metrics', json={
        'window': {'start_s': 50.0, 'end_s': 40.0}, 'metrics': [],
    })
    assert resp.status_code == 422


def test_unknown_metric_returns_400(client):
    resp = client.post('/v1/recordings/fixture-rec/metrics', json={
        'window': {'start_s': 0.0, 'end_s': 20.0},
        'metrics': [{'metric': 'nope', 'channel': 'accel_mag'}],
    })
    assert resp.status_code == 400


def test_window_too_short_is_a_per_metric_error_not_a_request_failure(client):
    resp = client.post('/v1/recordings/fixture-rec/metrics', json={
        'window': {'start_s': 200.0, 'end_s': 205.0},
        'metrics': [{'metric': 'plmi', 'channel': 'accel_mag'}],
    })
    assert resp.status_code == 200
    result = resp.json()['results'][0]
    assert result['status'] == 'error'
    assert result['error']['code'] == 'window_too_short'


def test_channel_unavailable_when_accel1_axes_not_in_store(client):
    resp = client.post('/v1/recordings/fixture-rec/metrics', json={
        'window': {'start_s': 200.0, 'end_s': 340.0},
        'metrics': [{'metric': 'plmi', 'channel': 'accel1_mag'}],
    })
    assert resp.status_code == 200
    result = resp.json()['results'][0]
    assert result['status'] == 'error'
    assert result['error']['code'] == 'channel_unavailable'


def test_cors_allows_any_origin(client):
    resp = client.options('/v1/recordings/fixture-rec/metrics', headers={
        'Origin': 'http://localhost:5555',
        'Access-Control-Request-Method': 'POST',
    })
    assert resp.headers.get('access-control-allow-origin') == '*'
