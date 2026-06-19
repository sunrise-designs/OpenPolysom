import json
import shutil
import socket
import subprocess
import webbrowser
from http.server import HTTPServer, SimpleHTTPRequestHandler
from pathlib import Path

import numpy as np
import zarr


def _git_short_hash():
    try:
        return subprocess.check_output(
            ['git', 'rev-parse', '--short', 'HEAD'],
            cwd=Path(__file__).parent,
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
    except Exception:
        return 'unknown'


def _load_patient():
    p = Path(__file__).parent / 'patient.json'
    if p.exists():
        return json.loads(p.read_text())
    return None


def save_zarr_json(stem, t, rr, accel_raw, accel_mag, hrv_t, hrv_rmssd,
                   stats, recording_meta):
    """Write time series to a Zarr v2 store and metadata to a JSON sidecar.

    Parameters
    ----------
    stem        : Path or str — output path without extension (e.g. 'biometric')
    t           : list[float]  — time axis in seconds
    rr          : list[float]  — RR interval series in ms
    accel_raw   : tuple(list, list, list) — raw X, Y, Z channels (uint8 0–255)
    accel_mag   : list[float]  — baseline-removed vector magnitude
    hrv_t       : array-like   — HRV window time axis in seconds (may be empty)
    hrv_rmssd   : array-like   — HRV RMSSD per window in ms (may be empty)
    stats       : dict          — output of count_plm() plus threshold/window_sec/fs
    recording_meta : dict | None — sidecar JSON from load_recording_meta()
    """
    stem = Path(stem)
    zarr_path = stem.with_suffix('.zarr')
    meta_path = Path(str(stem) + '_meta.json')

    # ── Zarr store (v2 format for zarr.js compatibility) ───────────────────────
    # zarr 3.x rmtree fails on Windows if the dir already exists — delete first.
    if zarr_path.exists():
        shutil.rmtree(zarr_path)
    root = zarr.open_group(str(zarr_path), mode='w', zarr_format=2)

    arrays = {
        't':         np.asarray(t,            dtype=np.float32),
        'rr':        np.asarray(rr,           dtype=np.float32),
        'accel_x':   np.asarray(accel_raw[0], dtype=np.uint8),
        'accel_y':   np.asarray(accel_raw[1], dtype=np.uint8),
        'accel_z':   np.asarray(accel_raw[2], dtype=np.uint8),
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

    # ── JSON sidecar ───────────────────────────────────────────────────────────
    meta = {
        'patient':        _load_patient(),
        'recording':      recording_meta,
        'stats': {
            'total_lms':   stats.get('total_lms', 0),
            'total_plms':  stats.get('total_plms', 0),
            'plmi':        stats.get('plmi', 0.0),
            'total_hours': stats.get('total_hours', 0.0),
            'hrv_overall': stats.get('hrv_overall', None),
            'threshold':   stats.get('threshold', None),
            'window_sec':  stats.get('window_sec', None),
            'fs':          stats.get('fs', 10),
        },
        'lm_events':  [[float(a), float(b)] for a, b in (stats.get('lm_events') or [])],
        'plm_groups': [[[float(a), float(b)] for a, b in grp] for grp in (stats.get('plm_groups') or [])],
        'git_hash':   _git_short_hash(),
        'zarr_path':  zarr_path.name,
    }
    meta_path.write_text(json.dumps(meta, indent=2), encoding='utf-8')
    print(f"Saved metadata to {meta_path}")

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
            return str(Path(output_dir) / path)

        def log_message(self, fmt, *args):
            pass  # suppress per-request noise

    with socket.socket() as s:
        s.bind(('', 0))
        port = s.getsockname()[1]

    server = HTTPServer(('localhost', port), DualDirHandler)
    url = f'http://localhost:{port}/index.html?meta={meta_filename}'
    print(f"Serving at {url}  (Ctrl-C to stop)")
    webbrowser.open(url)

    try:
        server.serve_forever()   # blocks main thread; Ctrl-C raises KeyboardInterrupt
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
