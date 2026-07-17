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
    print(f"[patient] {p} not found — subject.pii will be omitted from meta.json. "
          f"Copy patient.example.json to patient.json to include it.")
    return None


def _build_scoring(scoring_id, channel, stats):
    """Shape one accelerometer/combined scoring's lm_events/plm_groups
    (second-tuples) into one `scorings[]` entry, tagged with its source
    `channel` (the canonical Zarr array name) so a consumer can tell which
    chart pane a given event/group belongs to."""
    lm_events = stats.get('lm_events') or []
    plm_groups = stats.get('plm_groups') or []

    events = []
    id_of = {}
    for i, (on, off) in enumerate(lm_events, start=1):
        ev_id = f'{scoring_id}_ev_{i:04d}'
        id_of[(round(float(on), 4), round(float(off), 4))] = ev_id
        events.append({
            'id': ev_id, 'type': 'limb_movement',
            'onset_s': float(on), 'duration_s': float(off) - float(on),
            'channels': [channel],
        })

    groups = []
    for gi, grp in enumerate(plm_groups, start=1):
        members = [id_of[(round(float(a), 4), round(float(b), 4))] for a, b in grp]
        on0 = float(grp[0][0])
        off_last = float(grp[-1][1])
        groups.append({
            'id': f'{scoring_id}_grp_plm_{gi:03d}', 'type': 'plm_series',
            'member_ids': members, 'onset_s': on0, 'duration_s': off_last - on0,
            'channels': [channel],
            'params': {'n_movements': len(members)},
        })

    return {'scoring_id': scoring_id, 'events': events, 'groups': groups}


def _build_events(recording_id, t0_iso, stats, leg_stats=None):
    """Combine the bilateral (combined) scoring plus, when available, each
    accelerometer's own scoring into the events.json schema — one
    `scorings[]` entry per channel, each event/group tagged with its
    `channels` so the viewer can filter which pane a span belongs to.

    With no `leg_stats` (single-accelerometer callers), `stats` itself is the
    only accelerometer scored — tag it `accel_mag` (the sole array written),
    not `accel_combined_mag`, since nothing was actually combined.
    """
    if leg_stats is None:
        scorings = [_build_scoring('plm-auto-v0', 'accel_mag', stats)]
    else:
        scorings = [_build_scoring('plm-auto-v0-combined', 'accel_combined_mag', stats)]
        if leg_stats.get('accel0') is not None:
            scorings.append(_build_scoring('plm-auto-v0-accel0', 'accel_mag', leg_stats['accel0']))
        if leg_stats.get('accel1') is not None:
            scorings.append(_build_scoring('plm-auto-v0-accel1', 'accel1_mag', leg_stats['accel1']))

    return {
        'schema': 'protosom.events',
        'schema_version': SCHEMA_VERSIONS['events'],
        'recording_id': recording_id,
        'time_base': {'reference': 'recording_start', 'start_iso': t0_iso, 'unit': 's'},
        'channels_ref': 'working.zarr',
        'scorings': scorings,
    }


def save_zarr_json(stem, t, rr, accel_raw, accel_mag, hrv_t, hrv_rmssd,
                   stats, recording_meta, source_path=None, skip_s=0.0, rr_t=None,
                   accel1_mag=None, leg_stats=None, accel1_raw=None, respiratory_raw=None):
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
    accel_mag   : list[float]  — Accel0's baseline-removed vector magnitude (or, when
                  `leg_stats` is given, the combined bilateral envelope — see `stats`)
    hrv_t       : array-like   — HRV window time axis in seconds (may be empty)
    hrv_rmssd   : array-like   — HRV RMSSD per window in ms (may be empty)
    stats       : dict          — output of count_plm()/combine_bilateral_vm() plus
                  threshold/window_sec/fs — the headline (combined, when two
                  accelerometers are scored) result
    recording_meta : dict | None — sidecar JSON from load_recording_meta()
    source_path : Path | str | None — the EDF+ raw anchor this recording was read from
    skip_s      : float         — seconds trimmed from the start before DSP (provenance)
    accel1_mag  : list[float] | None — Accel1's baseline-removed vector magnitude, when
                  a second (wrist/ankle) accelerometer was scored
    leg_stats   : dict | None   — {'accel0': count_plm() result, 'accel1': count_plm()
                  result} — the per-leg breakdown backing `meta.json`'s `stats.legs` and
                  each leg's own `events.json` scoring
    accel1_raw  : tuple(list, list, list) | None — raw Accel1 X, Y, Z channels (physical
                  units), parallel to `accel_raw`. Written as `accel1_x/y/z` so a
                  windowed-metrics service can recompute PLM for Accel1/combined over an
                  arbitrary sub-window (baseline removal isn't correctly re-runnable on
                  the already-filtered `accel1_mag` trace for a new, smaller window).
    respiratory_raw : tuple(list, list, list) | None — Thoracic, Abdomen, Flow (physical
                  units: nH, nH, mbar), present on the RPi5 6-channel and ESP32-C6
                  11-channel devices, absent on wrist-only logs. Written as
                  `thoracic`/`abdomen`/`flow`, sharing the accelerometer time axis `t`
                  (same 50 Hz source rate, trimmed identically). No DSP applied yet — the
                  raw physical-unit trace, for display only.
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
        'accel_mag': np.asarray(leg_stats['accel0']['vm'] if leg_stats is not None else accel_mag, dtype=np.float32),
        'hrv_t':     np.asarray(hrv_t,     dtype=np.float32) if hrv_t is not None and len(hrv_t) > 0     else np.array([], dtype=np.float32),
        'hrv_rmssd': np.asarray(hrv_rmssd, dtype=np.float32) if hrv_rmssd is not None and len(hrv_rmssd) > 0 else np.array([], dtype=np.float32),
    }
    if accel1_mag is not None:
        arrays['accel1_mag'] = np.asarray(accel1_mag, dtype=np.float32)
    if accel1_raw is not None:
        arrays['accel1_x'] = np.asarray(accel1_raw[0], dtype=np.float32)
        arrays['accel1_y'] = np.asarray(accel1_raw[1], dtype=np.float32)
        arrays['accel1_z'] = np.asarray(accel1_raw[2], dtype=np.float32)
    if respiratory_raw is not None:
        arrays['thoracic'] = np.asarray(respiratory_raw[0], dtype=np.float32)
        arrays['abdomen'] = np.asarray(respiratory_raw[1], dtype=np.float32)
        arrays['flow'] = np.asarray(respiratory_raw[2], dtype=np.float32)
    if leg_stats is not None:
        # `accel_mag` above stays Accel0's own trace; the combined bilateral
        # envelope (what `stats`/`accel_mag` used to mean before both legs
        # were scored) gets its own array so neither leg's trace is lost.
        arrays['accel_combined_mag'] = np.asarray(accel_mag, dtype=np.float32)
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
            'total_gpcs':         stats.get('total_gpcs', 0),
            'total_hours':        stats.get('total_hours', 0.0),
            'hrv_rmssd_overall':  hrv_val,
            'threshold':          stats.get('threshold'),
            # The two GPC-rejection gates. Recorded because they change the LM
            # count: a reader cannot interpret `total_lms` without knowing what
            # was excluded from it. null = that gate was disabled for this run.
            'max_threshold':      stats.get('max_threshold'),
            'tilt_threshold_deg': stats.get('tilt_threshold_deg'),
            'window_sec':         stats.get('window_sec'),
            'fs':                 fs,
            **({'legs': {
                leg: {
                    'plmi':        leg_stats[leg].get('plmi', 0.0),
                    'total_lms':   leg_stats[leg].get('total_lms', 0),
                    'total_plms':  leg_stats[leg].get('total_plms', 0),
                    'total_gpcs':  leg_stats[leg].get('total_gpcs', 0),
                    'total_hours': leg_stats[leg].get('total_hours', 0.0),
                } for leg in ('accel0', 'accel1') if leg_stats.get(leg) is not None
            }} if leg_stats is not None else {}),
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

    events_doc = _build_events(recording_id, start_iso, stats, leg_stats=leg_stats)
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
