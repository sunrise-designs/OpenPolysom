import json
import subprocess
from datetime import datetime, timedelta
from pathlib import Path
import numpy as np
import plotly.graph_objects as go
from plotly.subplots import make_subplots

PLOTLY_HTML_OUT = 'biometric_plotly.html'

_GITHUB_URL  = 'https://github.com/sunrise-designs/ProtoSom'
_NETLIFY_URL = 'https://polysom.netlify.app/'
_HRV_URL     = 'https://en.wikipedia.org/wiki/Heart_rate_variability'
_AASM_URL    = 'https://polysom.netlify.app/AASM-Manual-2012.html'


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


def _build_title(stats, recording_meta):
    _commit = _git_short_hash()
    _links  = (f'<a href="{_NETLIFY_URL}">polysom.netlify.app</a>'
               f' &nbsp;|&nbsp; '
               f'<a href="{_GITHUB_URL}">GitHub</a> @ {_commit}')
    patient = _load_patient()
    if patient:
        title = (f"Biometric log &nbsp;|&nbsp; "
                 f"<b>{patient['name']}</b> &nbsp; "
                 f"DOB: {patient['dob']} &nbsp; "
                 f"NHS: {patient['nhs_number']} &nbsp; "
                 f"{patient['email']} &nbsp;|&nbsp; {_links}")
    else:
        title = f'Biometric log &nbsp;|&nbsp; {_links}'

    if recording_meta:
        title += (f"<br><sup>"
                  f"Date: {recording_meta['date']} &nbsp;|&nbsp; "
                  f"Legs: {recording_meta['legs']} &nbsp;|&nbsp; "
                  f"{recording_meta['start_time']} – {recording_meta['end_time']}"
                  f"</sup>")

    if stats:
        hrv_str = (f" &nbsp;|&nbsp; HRV (RMSSD): {stats['hrv_overall']:.1f} ms"
                   if 'hrv_overall' in stats and not np.isnan(stats['hrv_overall']) else '')
        params = (f" &nbsp;|&nbsp; Threshold: {stats['threshold']}"
                  f" &nbsp;|&nbsp; Window: {stats['window_sec']} s"
                  f" &nbsp;|&nbsp; {stats['fs']} Hz") if 'threshold' in stats else ''
        title += (f"<br><sup>LMs: {stats['total_lms']} &nbsp;|&nbsp; "
                  f"PLMs: {stats['total_plms']} &nbsp;|&nbsp; "
                  f"PLMI: {stats['plmi']:.1f} /hour"
                  f"{hrv_str}{params}</sup>")
    return title


def save_html(t, rr, accel_mag,
              lm_events=None, plm_groups=None, stats=None, recording_meta=None,
              raw_channels=None, include_plotlyjs=True):
    title = _build_title(stats, recording_meta)

    base  = datetime(1970, 1, 1)
    t_dt  = [base + timedelta(seconds=s) for s in t]
    to_dt = lambda s: base + timedelta(seconds=s)

    # RR: keep only transition points to reduce HTML size
    rr_arr     = np.array(rr)
    change_idx = np.concatenate(([0], np.where(np.diff(rr_arr) != 0)[0] + 1, [len(rr_arr) - 1]))
    t_rr_dt    = [t_dt[i] for i in change_idx]
    rr_plot    = rr_arr[change_idx].tolist()
    # Accel: every 3rd sample → ~3.3 Hz
    t_acc_dt   = t_dt[::3]
    acc_plot   = accel_mag[::3]

    hrv_t     = stats.get('hrv_t')     if stats else None
    hrv_rmssd = stats.get('hrv_rmssd') if stats else None
    has_hrv   = hrv_t is not None and len(hrv_t) > 0

    n_rows = 3 if has_hrv else 2
    subplot_titles = (
        'RR (ms)',
        f'Accel magnitude — <a href="{_AASM_URL}">AASM PLMD criteria</a>',
    )
    if has_hrv:
        subplot_titles += (
            f'HRV RMSSD (ms) — 5 min window — <a href="{_HRV_URL}">Wikipedia</a>',
        )

    fig = make_subplots(rows=n_rows, cols=1, shared_xaxes=True,
                        subplot_titles=subplot_titles)

    fig.add_trace(go.Scatter(x=t_rr_dt, y=rr_plot, mode='lines',
                             line=dict(color='red'), line_shape='hv'), row=1, col=1)
    fig.add_trace(go.Scatter(x=t_acc_dt, y=acc_plot, mode='lines',
                             line=dict(color='blue')), row=2, col=1)

    if has_hrv:
        fig.add_trace(go.Scatter(x=[to_dt(s) for s in hrv_t], y=hrv_rmssd,
                                 mode='lines', line=dict(color='purple')), row=3, col=1)

    if plm_groups:
        for group in plm_groups:
            fig.add_vrect(x0=to_dt(group[0][0]), x1=to_dt(group[-1][1]),
                          fillcolor='rgba(255,0,0,0.08)', line_color='red', line_width=1.5,
                          annotation_text='PLM series', annotation_position='top left',
                          annotation=dict(font_size=10, font_color='red'),
                          row=2, col=1)

    if lm_events:
        for onset, offset in lm_events:
            fig.add_vrect(x0=to_dt(onset), x1=to_dt(offset),
                          fillcolor='rgba(0,180,0,0.25)', line_color='green', line_width=1,
                          y0=0.1, y1=0.9, row=2, col=1)

    lo, hi = min(accel_mag), max(accel_mag)
    pad = max((hi - lo) * 0.05, 1)
    fig.update_yaxes(range=[lo - pad, hi + pad], row=2, col=1)
    fig.update_xaxes(tickformat='%H:%M', hoverformat='%H:%M:%S',
                     title_text='Time', row=n_rows, col=1)
    height = 700 if has_hrv else 550
    fig.update_layout(title=title, showlegend=False, height=height, margin=dict(t=140))

    fig.write_html(PLOTLY_HTML_OUT, include_plotlyjs=include_plotlyjs)
    print(f"Saved interactive chart to {PLOTLY_HTML_OUT}")
