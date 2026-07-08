import hashlib
import json
import math
import shutil
import socket
import subprocess
import webbrowser
from datetime import datetime, timezone
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
from pathlib import Path

import numpy as np
import zarr

SCHEMA_VERSIONS = {'meta': '0.1.0', 'events': '1.0.0', 'zarr_arrays': '0.1.0'}


def _git_provenance():
    def run(*a):
        return subprocess.check_output(
            ['git', *a], cwd=Path(__file__).parent, stderr=subprocess.DEVNULL, text=True,
        ).strip()
    try:
        return {
            'sha': run('rev-parse', 'HEAD'),
            'dirty': bool(run('status', '--porcelain')),
            'branch': run('rev-parse', '--abbrev-ref', 'HEAD'),
        }
    except Exception:
        return {'sha': 'unknown', 'dirty': True, 'branch': 'unknown'}


def _hash_file(path):
    data = Path(path).read_bytes()
    try:
        import blake3
        return {'algorithm': 'blake3', 'value': blake3.blake3(data).hexdigest()}
    except Exception:
        return {'algorithm': 'sha256', 'value': hashlib.sha256(data).hexdigest()}


def _load_patient():
    p = Path(__file__).parent / 'patient.json'
    if p.exists():
        return json.loads(p.read_text())
    return None


def _build_events(recording_id, t0_iso, stats):
    """Shape lm_events/plm_groups (second-tuples) into the events.json schema."""
    lm_events = stats.get('lm_events') or []
    plm_groups = stats.get('plm_groups') or []

    events = []
    id_of = {}
    for i, (on, off) in enumerate(lm_events, start=1):
        ev_id = f'ev_{i:04d}'
        id_of[(round(float(on), 4), round(float(off), 4))] = ev_id
        events.append({
            'id': ev_id, 'type': 'limb_movement',
            'onset_s': float(on), 'duration_s': float(off) - float(on),
            'channels': ['accel_mag'],
        })

    groups = []
    for gi, grp in enumerate(plm_groups, start=1):
        members = [id_of[(round(float(a), 4), round(float(b), 4))] for a, b in grp]
        on0 = float(grp[0][0])
        off_last = float(grp[-1][1])
        groups.append({
            'id': f'grp_plm_{gi:03d}', 'type': 'plm_series',
            'member_ids': members, 'onset_s': on0, 'duration_s': off_last - on0,
            'params': {'n_movements': len(members)},
        })

    return {
        'schema': 'protosom.events',
        'schema_version': SCHEMA_VERSIONS['events'],
        'recording_id': recording_id,
        'time_base': {'reference': 'recording_start', 'start_iso': t0_iso, 'unit': 's'},
        'channels_ref': 'working.zarr',
        'scorings': [{'scoring_id': 'plm-auto-v0', 'events': events, 'groups': groups}],
    }


def save_zarr_json(stem, t, rr, accel_raw, accel_mag, hrv_t, hrv_rmssd,
                   stats, recording_meta, source_path=None, skip_s=0.0, rr_t=None):
    """Write time series to a Zarr v2 store and metadata + events to JSON sidecars.

    Parameters
    ----------
    stem        : Path or str — output path without extension (e.g. 'biometric')
    t           : list[float]  — accelerometer time axis in seconds
    rr          : list[float]  — RR interval series in ms
    rr_t        : list[float] | None — RR's own time axis in seconds (RR is sampled at
                  its own native rate, distinct from the accelerometer's; falls back to
                  `t` if omitted, which is only correct when the rates already match)
    accel_raw   : tuple(list, list, list) — raw X, Y, Z channels (physical units, e.g. mg)
    accel_mag   : list[float]  — baseline-removed vector magnitude
    hrv_t       : array-like   — HRV window time axis in seconds (may be empty)
    hrv_rmssd   : array-like   — HRV RMSSD per window in ms (may be empty)
    stats       : dict          — output of count_plm() plus threshold/window_sec/fs
    recording_meta : dict | None — sidecar JSON from load_recording_meta()
    source_path : Path | str | None — the EDF+ raw anchor this recording was read from
    skip_s      : float         — seconds trimmed from the start before DSP (provenance)
    """
    stem = Path(stem)
    zarr_path = stem.with_suffix('.zarr')
    meta_path = Path(str(stem) + '_meta.json')
    events_path = meta_path.parent / 'events.json'

    # ── Zarr store (v2 format for zarrita.js compatibility) ────────────────────
    # zarr 3.x rmtree fails on Windows if the dir already exists — delete first.
    if zarr_path.exists():
        shutil.rmtree(zarr_path)
    root = zarr.open_group(str(zarr_path), mode='w', zarr_format=2)

    arrays = {
        't':         np.asarray(t,            dtype=np.float32),
        'rr':        np.asarray(rr,           dtype=np.float32),
        'rr_t':      np.asarray(rr_t if rr_t is not None else t[:len(rr)], dtype=np.float32),
        'accel_x':   np.asarray(accel_raw[0], dtype=np.float32),
        'accel_y':   np.asarray(accel_raw[1], dtype=np.float32),
        'accel_z':   np.asarray(accel_raw[2], dtype=np.float32),
        'accel_mag': np.asarray(accel_mag,    dtype=np.float32),
        'hrv_t':     np.asarray(hrv_t,     dtype=np.float32) if hrv_t is not None and len(hrv_t) > 0     else np.array([], dtype=np.float32),
        'hrv_rmssd': np.asarray(hrv_rmssd, dtype=np.float32) if hrv_rmssd is not None and len(hrv_rmssd) > 0 else np.array([], dtype=np.float32),
    }
    for name, data in arrays.items():
        # Single chunk per array: zarr 3.x has async overhead per chunk, so
        # writing the whole array as one chunk keeps write time O(1) in calls.
        arr = root.create_array(name, shape=data.shape or (0,), dtype=data.dtype,
                                chunks=data.shape or (1,))
        if data.size > 0:
            arr[...] = data

    print(f"Saved Zarr store to {zarr_path}")

    # ── meta.json (schema per wiki/planning/zarr-schema-spec.md §6) ────────────
    fs = stats.get('fs') or 50
    n_samples = len(t)
    duration_s = stats.get('total_hours', n_samples / fs / 3600) * 3600
    recording_meta = recording_meta or {}
    start_iso = recording_meta.get('recording_start_time') or datetime.now(timezone.utc).isoformat()
    recording_id = stem.name
    subject_id = recording_meta.get('device_uid') or 'PSG-0001'

    subject = {'subject_id': subject_id, 'sex': None, 'dob': None}
    patient = _load_patient()
    if patient is not None:
        # PII stays in a separable block, never inlined into signal/summary fields.
        subject['pii'] = patient

    raw_biosignals = []
    if source_path is not None and Path(source_path).exists():
        raw_biosignals.append({
            'path': Path(source_path).name, 'format': 'EDF+', 'hash': _hash_file(source_path),
        })

    hrv_raw = stats.get('hrv_overall')
    hrv_val = None if hrv_raw is None or (isinstance(hrv_raw, float) and math.isnan(hrv_raw)) else hrv_raw

    meta = {
        'schema_versions': SCHEMA_VERSIONS,
        'recording_id': recording_id,
        'subject': subject,
        'recording': {
            'start_iso': start_iso,
            'duration_s': duration_s,
            'n_samples': n_samples,
            'sample_rate_hz': fs,
            'source': subject_id,
        },
        'stats': {
            'plmi':               stats.get('plmi', 0.0),
            'total_lms':          stats.get('total_lms', 0),
            'total_plms':         stats.get('total_plms', 0),
            'total_hours':        stats.get('total_hours', 0.0),
            'hrv_rmssd_overall':  hrv_val,
            'threshold':          stats.get('threshold'),
            'window_sec':         stats.get('window_sec'),
            'fs':                 fs,
        },
        'layers': {
            'raw':     {'biosignals': raw_biosignals},
            'working': {'path': zarr_path.name, 'zarr_format': 2},
        },
        'provenance': {
            'generated_at': datetime.now(timezone.utc).isoformat(),
            'pipeline': {'repo': 'sunrise-designs/ProtoSom', 'git': _git_provenance()},
            'input_preparation': {
                'skip_samples': int(skip_s * fs),
                'note': f'leading {skip_s:.1f} s dropped before DSP',
            },
        },
    }
    meta_path.write_text(json.dumps(meta, indent=2), encoding='utf-8')
    print(f"Saved metadata to {meta_path}")

    events_doc = _build_events(recording_id, start_iso, stats)
    events_path.write_text(json.dumps(events_doc, indent=2), encoding='utf-8')
    print(f"Saved events to {events_path}")

    return zarr_path, meta_path


def serve_and_open(output_dir, meta_filename, src_web_dir=None):
    """Start a local HTTP server and open the viewer in the browser.

    Blocks until Ctrl-C — run this as the last step in the script.
    """
    if src_web_dir is None:
        src_web_dir = Path(__file__).parent.parent / 'src_web'

    index_src    = src_web_dir / 'index.html'
    chart_js_src = src_web_dir / 'dist' / 'chart.js'

    class DualDirHandler(SimpleHTTPRequestHandler):
        def translate_path(self, path):
            path = path.split('?', 1)[0].lstrip('/')
            if not path or path == 'index.html':
                return str(index_src)
            if path in ('dist/chart.js', 'chart.js'):
                return str(chart_js_src)
            # Static assets (styles.css, sw.js, manifest, icons, ...) live beside
            # index.html in src_web/, not in the per-recording output dir.
            static_candidate = src_web_dir / path
            if static_candidate.is_file():
                return str(static_candidate)
            return str(Path(output_dir) / path)

        def log_message(self, fmt, *args):
            pass  # suppress per-request noise

    with socket.socket() as s:
        s.bind(('', 0))
        port = s.getsockname()[1]

    # Threaded: the viewer opens many concurrent requests (one per Zarr array,
    # each needing .zattrs + a chunk), which a single-threaded HTTPServer's
    # small connection backlog can't keep up with — Chrome sees the overflow
    # as ERR_CONNECTION_REFUSED.
    server = ThreadingHTTPServer(('localhost', port), DualDirHandler)
    url = f'http://localhost:{port}/index.html?meta={meta_filename}'
    print(f"Serving at {url}  (Ctrl-C to stop)")
    webbrowser.open(url)

    try:
        server.serve_forever()   # blocks main thread; Ctrl-C raises KeyboardInterrupt
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
