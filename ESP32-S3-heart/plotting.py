import json
import struct
from datetime import datetime, timedelta
from pathlib import Path
import numpy as np
import plotly.graph_objects as go
from plotly.subplots import make_subplots

PLOTLY_HTML_OUT = 'biometric_plotly.html'


def _load_patient():
    p = Path(__file__).parent / 'patient.json'
    if p.exists():
        return json.loads(p.read_text())
    return None


def save_plotly_html(t, rr, accel_x, accel_y, accel_z,
                     lm_events=None, plm_groups=None, stats=None, recording_meta=None):
    patient = _load_patient()
    if patient:
        title = (f"Biometric log &nbsp;|&nbsp; "
                 f"<b>{patient['name']}</b> &nbsp; "
                 f"DOB: {patient['dob']} &nbsp; "
                 f"NHS: {patient['nhs_number']} &nbsp; "
                 f"{patient['email']}")
    else:
        title = 'Biometric log'

    if recording_meta:
        title += (f"<br><sup>"
                  f"Date: {recording_meta['date']} &nbsp;|&nbsp; "
                  f"Legs: {recording_meta['legs']} &nbsp;|&nbsp; "
                  f"{recording_meta['start_time']} – {recording_meta['end_time']}"
                  f"</sup>")

    hrv_t     = stats.get('hrv_t')     if stats else None
    hrv_rmssd = stats.get('hrv_rmssd') if stats else None
    has_hrv   = hrv_t is not None and len(hrv_t) > 0

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

    base  = datetime(1970, 1, 1)
    t_dt  = [base + timedelta(seconds=s) for s in t]
    to_dt = lambda s: base + timedelta(seconds=s)

    n_rows = 5 if has_hrv else 4
    subplot_titles = ('RR (ms)', 'Accel X', 'Accel Y', 'Accel Z')
    if has_hrv:
        subplot_titles += ('HRV RMSSD (ms) — 5 min window',)

    fig = make_subplots(rows=n_rows, cols=1, shared_xaxes=True,
                        subplot_titles=subplot_titles)

    fig.add_trace(go.Scatter(x=t_dt, y=rr,      mode='lines', line=dict(color='red')),    row=1, col=1)
    fig.add_trace(go.Scatter(x=t_dt, y=accel_x, mode='lines', line=dict(color='blue')),   row=2, col=1)
    fig.add_trace(go.Scatter(x=t_dt, y=accel_y, mode='lines', line=dict(color='green')),  row=3, col=1)
    fig.add_trace(go.Scatter(x=t_dt, y=accel_z, mode='lines', line=dict(color='orange')), row=4, col=1)

    if has_hrv:
        fig.add_trace(go.Scatter(x=[to_dt(s) for s in hrv_t], y=hrv_rmssd,
                                 mode='lines', line=dict(color='purple')), row=5, col=1)

    # PLM series boxes drawn first so LM boxes appear on top
    if plm_groups:
        for group in plm_groups:
            fig.add_vrect(x0=to_dt(group[0][0]), x1=to_dt(group[-1][1]),
                          fillcolor='rgba(255,0,0,0.08)', line_color='red', line_width=1.5,
                          annotation_text='PLM series', annotation_position='top left',
                          annotation=dict(font_size=10, font_color='red'))

    if lm_events:
        for onset, offset in lm_events:
            fig.add_vrect(x0=to_dt(onset), x1=to_dt(offset),
                          fillcolor='rgba(0,180,0,0.25)', line_color='green', line_width=1)

    for row, ch in enumerate([accel_x, accel_y, accel_z], start=2):
        lo, hi = min(ch), max(ch)
        pad = max((hi - lo) * 0.05, 1)
        fig.update_yaxes(range=[lo - pad, hi + pad], row=row, col=1)

    fig.update_xaxes(tickformat='%H:%M', hoverformat='%H:%M:%S', title_text='Time', row=n_rows, col=1)
    fig.update_layout(title=title, showlegend=False, height=900 if has_hrv else 800)

    fig.write_html(PLOTLY_HTML_OUT, include_plotlyjs=True)
    print(f"Saved interactive chart to {PLOTLY_HTML_OUT}")


def plot(data, recording_meta=None):
    records = [(data[i], data[i+1], data[i+2], struct.unpack_from('<H', data, i+3)[0])
               for i in range(0, len(data) - 4, 5)]
    t = [i / 10.0 for i in range(len(records))]
    save_plotly_html(
        t,
        [r[3] for r in records],
        [r[0] for r in records],
        [r[1] for r in records],
        [r[2] for r in records],
        recording_meta=recording_meta,
    )
