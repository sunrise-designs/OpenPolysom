import json
import subprocess
from datetime import datetime, timedelta
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
from matplotlib.backends.backend_pdf import PdfPages

PLOTLY_PDF_OUT = 'biometric_plotly.pdf'

_GITHUB_URL  = 'https://github.com/sunrise-designs/ProtoSom'
_NETLIFY_URL = 'https://polysom.netlify.app/'
_HRV_URL     = 'https://en.wikipedia.org/wiki/Heart_rate_variability'
_AASM_URL    = 'https://polysom.netlify.app/AASM-Manual-2012.html'

_LINK_COLOR = '#1155CC'


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
    print(f"[patient] {p} not found — the PDF report header will omit patient details. "
          f"Copy patient.example.json to patient.json to include them.")
    return None


def _draw_header(fig, stats, recording_meta):
    """Render the title block: plain text left, clickable links right."""
    _commit = _git_short_hash()
    patient = _load_patient()

    if patient:
        line1 = (f"Biometric log  |  {patient['name']}  "
                 f"DOB: {patient['dob']}  NHS: {patient['nhs_number']}  {patient['email']}")
    else:
        line1 = 'Biometric log'

    lines = [line1]
    if recording_meta:
        lines.append(f"Date: {recording_meta['date']}  |  "
                     f"Legs: {recording_meta['legs']}  |  "
                     f"{recording_meta['start_time']} – {recording_meta['end_time']}")
    if stats:
        hrv_str = (f"  |  HRV (RMSSD): {stats['hrv_overall']:.1f} ms"
                   if 'hrv_overall' in stats and not np.isnan(stats['hrv_overall']) else '')
        params = (f"  |  Threshold: {stats['threshold']}"
                  f"  |  Window: {stats['window_sec']} s"
                  f"  |  {stats['fs']} Hz") if 'threshold' in stats else ''
        lines.append(f"LMs: {stats['total_lms']}  |  "
                     f"PLMs: {stats['total_plms']}  |  "
                     f"PLMI: {stats['plmi']:.1f} /hour{hrv_str}{params}")

    fig.text(0.01, 0.99, '\n'.join(lines), va='top', ha='left', fontsize=8)

    # Clickable links — right-aligned in the header area
    fig.text(0.99, 0.99,  'polysom.netlify.app', va='top', ha='right',
             fontsize=8, color=_LINK_COLOR, url=_NETLIFY_URL)
    fig.text(0.99, 0.975, f'GitHub @ {_commit}', va='top', ha='right',
             fontsize=8, color=_LINK_COLOR, url=_GITHUB_URL)


def _ax_title_with_link(ax, plain, link_text, link_url):
    """Set an axes title: plain text left, clickable link right."""
    ax.text(0.0, 1.01, plain, transform=ax.transAxes,
            fontsize=9, va='bottom', ha='left')
    ax.text(1.0, 1.01, link_text, transform=ax.transAxes,
            fontsize=9, va='bottom', ha='right', color=_LINK_COLOR, url=link_url)


def save_pdf(t, rr, accel_mag,
             lm_events=None, plm_groups=None, stats=None, recording_meta=None,
             raw_channels=None):
    base  = datetime(1970, 1, 1)
    t_dt  = [base + timedelta(seconds=s) for s in t]
    to_dt = lambda s: base + timedelta(seconds=s)

    hrv_t     = stats.get('hrv_t')     if stats else None
    hrv_rmssd = stats.get('hrv_rmssd') if stats else None
    has_hrv   = hrv_t is not None and len(hrv_t) > 0

    n_rows = 3 if has_hrv else 2

    fig, axes = plt.subplots(n_rows, 1, figsize=(11.69, 8.27), sharex=True,
                             gridspec_kw={'hspace': 0.45})
    if n_rows == 1:
        axes = [axes]
    fig.subplots_adjust(top=0.88, bottom=0.08, left=0.07, right=0.98)
    _draw_header(fig, stats, recording_meta)

    # Row 1: RR interval (step plot, hv-style)
    ax_rr = axes[0]
    ax_rr.plot(t_dt, rr, color='red', linewidth=0.6, drawstyle='steps-post')
    ax_rr.set_ylabel('RR (ms)', fontsize=8)
    ax_rr.set_title('RR interval', fontsize=9, loc='left')
    ax_rr.yaxis.grid(True, alpha=0.3)
    ax_rr.tick_params(labelsize=7)

    # Row 2: Accel magnitude with LM/PLM overlays
    ax_acc = axes[1]
    ax_acc.plot(t_dt, accel_mag, color='#2255BB', linewidth=0.4)
    ax_acc.set_ylabel('Accel magnitude', fontsize=8)
    lo, hi = min(accel_mag), max(accel_mag)
    pad = max((hi - lo) * 0.05, 1)
    ax_acc.set_ylim(lo - pad, hi + pad)
    ax_acc.yaxis.grid(True, alpha=0.3)
    ax_acc.tick_params(labelsize=7)
    _ax_title_with_link(ax_acc, 'Accel magnitude', 'AASM PLMD criteria', _AASM_URL)

    # PLM series shading behind LM boxes
    if plm_groups:
        for group in plm_groups:
            ax_acc.axvspan(to_dt(group[0][0]), to_dt(group[-1][1]),
                           color='red', alpha=0.08)
    if lm_events:
        for onset, offset in lm_events:
            ax_acc.axvspan(to_dt(onset), to_dt(offset),
                           ymin=0.1, ymax=0.9, color='green', alpha=0.25)

    # Row 3 (optional): HRV RMSSD
    if has_hrv:
        ax_hrv = axes[2]
        ax_hrv.plot([to_dt(s) for s in hrv_t], hrv_rmssd,
                    color='purple', linewidth=0.6)
        ax_hrv.set_ylabel('RMSSD (ms)', fontsize=8)
        ax_hrv.yaxis.grid(True, alpha=0.3)
        ax_hrv.tick_params(labelsize=7)
        _ax_title_with_link(ax_hrv, 'HRV RMSSD — 5 min window', 'Wikipedia', _HRV_URL)

    axes[-1].xaxis.set_major_formatter(mdates.DateFormatter('%H:%M'))
    axes[-1].set_xlabel('Time', fontsize=8)
    fig.autofmt_xdate(rotation=0, ha='center')

    with PdfPages(PLOTLY_PDF_OUT) as pdf:
        pdf.savefig(fig, bbox_inches='tight')

        if raw_channels is not None:
            accel_x, accel_y, accel_z = raw_channels
            fig2, axes2 = plt.subplots(3, 1, figsize=(11.69, 8.27), sharex=True,
                                       gridspec_kw={'hspace': 0.35})
            fig2.subplots_adjust(top=0.94, bottom=0.08, left=0.07, right=0.98)
            fig2.suptitle('Raw accelerometer channels', fontsize=10, y=0.98)
            for ax, ch, color, label in zip(
                    axes2,
                    [accel_x, accel_y, accel_z],
                    ['blue', 'green', 'orange'],
                    ['X (raw)', 'Y (raw)', 'Z (raw)']):
                ax.plot(t_dt, ch, color=color, linewidth=0.4)
                ax.set_ylabel(label, fontsize=8)
                lo, hi = min(ch), max(ch)
                pad = max((hi - lo) * 0.05, 1)
                ax.set_ylim(lo - pad, hi + pad)
                ax.yaxis.grid(True, alpha=0.3)
                ax.tick_params(labelsize=7)
            axes2[-1].xaxis.set_major_formatter(mdates.DateFormatter('%H:%M'))
            axes2[-1].set_xlabel('Time', fontsize=8)
            fig2.autofmt_xdate(rotation=0, ha='center')
            pdf.savefig(fig2, bbox_inches='tight')
            plt.close(fig2)

    plt.close(fig)
    print(f"Saved PDF to {PLOTLY_PDF_OUT}")
