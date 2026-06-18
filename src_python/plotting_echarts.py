import base64
import json
from pathlib import Path

import numpy as np

from plotting_html import _build_title

ECHARTS_HTML_OUT = 'biometric_echarts.html'


def _encode_f32(arr):
    return base64.b64encode(np.asarray(arr, dtype=np.float32).tobytes()).decode()


def save_echarts_html(t, rr, accel_mag,
                      lm_events=None, plm_groups=None, stats=None, recording_meta=None,
                      raw_channels=None):
    """Self-contained ECharts export: data embedded as Float32 blobs (Zarr-style pipeline) with LTTB downsampling."""
    title_html = _build_title(stats, recording_meta)

    t_arr   = np.asarray(t,         dtype=np.float32)
    rr_arr  = np.asarray(rr,        dtype=np.float32)
    acc_arr = np.asarray(accel_mag, dtype=np.float32)

    # RR: step-function compression — keep only transition points
    chg = np.concatenate(([0], np.where(np.diff(rr_arr) != 0)[0] + 1, [len(rr_arr) - 1]))

    has_hrv   = bool(stats and stats.get('hrv_t') is not None and len(stats['hrv_t']) > 0)
    hrv_t_b64 = _encode_f32(stats['hrv_t'])     if has_hrv else ''
    hrv_v_b64 = _encode_f32(stats['hrv_rmssd']) if has_hrv else ''

    lm_js  = json.dumps([[float(a), float(b)] for a, b in lm_events] if lm_events else [])
    plm_js = json.dumps([[float(g[0][0]), float(g[-1][1])] for g in plm_groups] if plm_groups else [])

    raw_ch_js = 'null'
    if raw_channels:
        # Accept either [(name, arr), ...] or a plain tuple/list of arrays
        if isinstance(raw_channels[0][0], str):
            pairs = list(raw_channels)
        else:
            pairs = [(f'Ch {i}', ch) for i, ch in enumerate(raw_channels)]
        ch_list = [{'name': name, 't': _encode_f32(t_arr),
                    'v': _encode_f32(np.asarray(sig, dtype=np.float32))}
                   for name, sig in pairs]
        raw_ch_js = json.dumps(ch_list)

    t_rr_b64  = _encode_f32(t_arr[chg])
    rr_b64    = _encode_f32(rr_arr[chg])
    t_acc_b64 = _encode_f32(t_arr[::3])
    acc_b64   = _encode_f32(acc_arr[::3])

    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Biometric log — ECharts</title>
<script src="https://cdn.jsdelivr.net/npm/echarts@5.5.0/dist/echarts.min.js"></script>
<style>
  * {{ user-select: text !important; -webkit-user-select: text !important; }}
  body {{ margin: 0; padding: 0; background: #111; color: #eee; font-family: system-ui, sans-serif; }}
  #title {{ padding: 6px 16px; font-size: 13px; line-height: 1.6; border-bottom: 1px solid #333; }}
  #chart {{ width: 100vw; height: calc(100vh - 70px); }}
</style>
</head>
<body>
<div id="title"></div>
<div id="chart"></div>
<script>
document.getElementById('title').innerHTML = {json.dumps(title_html)};

// ── Zarr-style binary data pipeline ─────────────────────────────────────────────
// In a live viewer each blob is fetched as an ArrayBuffer via HTTP Range request:
//   const resp = await fetch(zarrChunkUrl, {{ headers: {{ Range: 'bytes=0-' }} }});
//   const f32  = new Float32Array(await resp.arrayBuffer());
// Here the chunk payload is embedded as base64 for a fully self-contained export.
// The ArrayBuffer → Float32Array cast and LTTB pipeline are identical either way.

function b64ToF32(b64) {{
    if (!b64) return new Float32Array(0);
    const bin = atob(b64);
    const buf = new ArrayBuffer(bin.length);
    const u8  = new Uint8Array(buf);
    for (let i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i);
    return new Float32Array(buf);  // zero-copy typed-array view over the same buffer
}}

// Decode embedded Float32 blobs (Zarr chunk payloads)
const T_RR  = b64ToF32('{t_rr_b64}');
const RR    = b64ToF32('{rr_b64}');
const T_ACC = b64ToF32('{t_acc_b64}');
const ACC   = b64ToF32('{acc_b64}');
const HAS_HRV = {str(has_hrv).lower()};
const T_HRV = b64ToF32('{hrv_t_b64}');
const V_HRV = b64ToF32('{hrv_v_b64}');
const LM_EVENTS  = {lm_js};
const PLM_GROUPS = {plm_js};
const RAW_CHANNELS = {raw_ch_js};

const BASE_MS = 0;  // Unix epoch (1970-01-01), mirrors Plotly export

function toMs(f32) {{
    const out = new Float64Array(f32.length);
    for (let i = 0; i < f32.length; i++) out[i] = BASE_MS + f32[i] * 1000;
    return out;
}}

function zip(xMs, yF32) {{
    const n = Math.min(xMs.length, yF32.length);
    const d = new Array(n);
    for (let i = 0; i < n; i++) d[i] = [xMs[i], yF32[i]];
    return d;
}}

const rrT  = toMs(T_RR);
const accT = toMs(T_ACC);
const hrvT = HAS_HRV ? toMs(T_HRV) : [];

const nExtra = RAW_CHANNELS ? RAW_CHANNELS.length : 0;
const nRows  = 2 + (HAS_HRV ? 1 : 0) + nExtra;
const rowH   = (85 / nRows).toFixed(1);

function makeGrid(r) {{
    const top = (5 + r * (parseFloat(rowH) + 2)).toFixed(1);
    return {{ left: 70, right: 20, top: top + '%', height: rowH + '%' }};
}}

const grids  = [];
const xAxes  = [];
const yAxes  = [];
const series = [];

const yNames = ['RR (ms)', 'Accel magnitude'];
if (HAS_HRV) yNames.push('HRV RMSSD (ms)');
if (RAW_CHANNELS) RAW_CHANNELS.forEach(ch => yNames.push(ch.name));

for (let r = 0; r < nRows; r++) {{
    grids.push(makeGrid(r));
    xAxes.push({{
        gridIndex: r, type: 'time',
        axisLabel: {{
            show: r === nRows - 1,
            formatter: v => new Date(v).toISOString().substr(11, 8),
            fontSize: 10,
        }},
        splitLine: {{ show: false }},
        axisLine: {{ lineStyle: {{ color: '#444' }} }},
    }});
    yAxes.push({{
        gridIndex: r, type: 'value',
        name: yNames[r], nameLocation: 'middle', nameGap: 55,
        nameTextStyle: {{ fontSize: 11, color: '#aaa' }},
        splitLine: {{ lineStyle: {{ color: '#222' }} }},
        axisLabel: {{ fontSize: 10 }},
    }});
}}

// markArea data for LM (green) and PLM (red) regions on the accel subplot
const lmMark  = LM_EVENTS.map(([a, b]) => [
    {{ xAxis: BASE_MS + a * 1000, itemStyle: {{ color: 'rgba(0,180,0,0.25)' }}, label: {{ show: false }} }},
    {{ xAxis: BASE_MS + b * 1000 }},
]);
const plmMark = PLM_GROUPS.map(([a, b]) => [
    {{ xAxis: BASE_MS + a * 1000,
       itemStyle: {{ color: 'rgba(220,0,0,0.10)', borderColor: '#dd0000', borderWidth: 1.5 }},
       label: {{ show: true, position: 'insideTopLeft', formatter: 'PLM', fontSize: 9, color: '#dd0000' }} }},
    {{ xAxis: BASE_MS + b * 1000 }},
]);

// RR interval — step line, LTTB downsampling
series.push({{
    name: 'RR (ms)', type: 'line', xAxisIndex: 0, yAxisIndex: 0,
    data: zip(rrT, RR), step: 'end',
    showSymbol: false, animation: false,
    lineStyle: {{ color: '#ff4444', width: 1 }},
    large: true, largeThreshold: 2000,
    sampling: 'lttb',
}});

// Accel magnitude — LTTB + LM/PLM regions
series.push({{
    name: 'Accel magnitude', type: 'line', xAxisIndex: 1, yAxisIndex: 1,
    data: zip(accT, ACC),
    showSymbol: false, animation: false,
    lineStyle: {{ color: '#4488ff', width: 1 }},
    large: true, largeThreshold: 5000,
    sampling: 'lttb',
    markArea: {{ silent: true, data: [...lmMark, ...plmMark] }},
}});

let si = 2;

if (HAS_HRV) {{
    series.push({{
        name: 'HRV RMSSD (ms)', type: 'line', xAxisIndex: si, yAxisIndex: si,
        data: zip(hrvT, V_HRV),
        showSymbol: false, animation: false,
        lineStyle: {{ color: '#cc66ff', width: 1.5 }},
        large: true, largeThreshold: 2000,
        sampling: 'lttb',
    }});
    si++;
}}

// Raw PSG channels — LTTB critical: preserves sleep spindles and epileptic spikes
const chColors = ['#00ffcc', '#ffaa00', '#ff66cc', '#66ffaa', '#ff8844'];
if (RAW_CHANNELS) {{
    RAW_CHANNELS.forEach((ch, ci) => {{
        series.push({{
            name: ch.name, type: 'line', xAxisIndex: si + ci, yAxisIndex: si + ci,
            data: zip(toMs(b64ToF32(ch.t)), b64ToF32(ch.v)),
            showSymbol: false, animation: false,
            lineStyle: {{ color: chColors[ci % chColors.length], width: 1 }},
            large: true, largeThreshold: 5000,
            sampling: 'lttb',
        }});
    }});
}}

const zIdxs = Array.from({{ length: nRows }}, (_, i) => i);

const chart = echarts.init(document.getElementById('chart'), 'dark', {{ renderer: 'canvas' }});
chart.setOption({{
    backgroundColor: '#111',
    animation: false,
    tooltip: {{
        trigger: 'axis',
        axisPointer: {{ animation: false, type: 'cross' }},
        formatter: params => {{
            if (!params.length) return '';
            const ts = new Date(params[0].value[0]).toISOString().substr(11, 8);
            return params.map(p => `${{p.seriesName}}: ${{Number(p.value[1]).toFixed(1)}}`).join('<br>') + `<br><b>${{ts}}</b>`;
        }},
    }},
    dataZoom: [
        {{ type: 'inside', xAxisIndex: zIdxs, filterMode: 'none' }},
        {{ type: 'slider',  xAxisIndex: zIdxs, filterMode: 'none', bottom: 5, height: 22 }},
    ],
    grid: grids, xAxis: xAxes, yAxis: yAxes, series,
}});

window.addEventListener('resize', () => chart.resize());
</script>
</body>
</html>"""

    Path(ECHARTS_HTML_OUT).write_text(html, encoding='utf-8')
    print(f'Saved ECharts interactive chart to {ECHARTS_HTML_OUT}')
