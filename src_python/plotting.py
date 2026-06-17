from plotting_html import save_html, PLOTLY_HTML_OUT  # noqa: F401
from plotting_pdf  import save_pdf,  PLOTLY_PDF_OUT   # noqa: F401


def save_plotly_html(t, rr, accel_mag,
                     lm_events=None, plm_groups=None, stats=None, recording_meta=None,
                     raw_channels=None, include_plotlyjs=True):
    save_html(t, rr, accel_mag,
              lm_events=lm_events, plm_groups=plm_groups, stats=stats,
              recording_meta=recording_meta, raw_channels=raw_channels,
              include_plotlyjs=include_plotlyjs)
    save_pdf(t, rr, accel_mag,
             lm_events=lm_events, plm_groups=plm_groups, stats=stats,
             recording_meta=recording_meta, raw_channels=raw_channels)


def plot(t, rr, accel_mag, recording_meta=None, raw_channels=None, include_plotlyjs=True):
    save_plotly_html(t, rr, accel_mag, recording_meta=recording_meta,
                     raw_channels=raw_channels, include_plotlyjs=include_plotlyjs)
