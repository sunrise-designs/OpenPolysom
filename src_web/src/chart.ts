import type { ZarrData, SidecarMeta } from './types';

const BASE_MS = 0; // seconds → ms offset from unix epoch for ECharts time axis

function toMs(f32: Float32Array | Uint8Array): Float64Array {
  const out = new Float64Array(f32.length);
  for (let i = 0; i < f32.length; i++) out[i] = BASE_MS + f32[i] * 1000;
  return out;
}

function zip(xMs: Float64Array, y: Float32Array | Uint8Array): [number, number][] {
  const n = Math.min(xMs.length, y.length);
  const d: [number, number][] = new Array(n);
  for (let i = 0; i < n; i++) d[i] = [xMs[i], y[i]];
  return d;
}

function formatTime(ms: number): string {
  return new Date(ms).toISOString().substring(11, 19);
}

function buildTitle(meta: SidecarMeta): string {
  const p = meta.patient;
  let title = p
    ? `Biometric log &nbsp;|&nbsp; <b>${p.name}</b> &nbsp; DOB: ${p.dob} &nbsp; NHS: ${p.nhs_number}`
    : 'Biometric log';

  const r = meta.recording;
  if (r) {
    title += `<br><small>Date: ${r.date} &nbsp;|&nbsp; Legs: ${r.legs} &nbsp;|&nbsp; ${r.start_time} – ${r.end_time}</small>`;
  }

  const s = meta.stats;
  const hrv = (s.hrv_overall != null && !isNaN(s.hrv_overall))
    ? ` &nbsp;|&nbsp; HRV (RMSSD): ${s.hrv_overall.toFixed(1)} ms` : '';
  title += `<br><small>LMs: ${s.total_lms} &nbsp;|&nbsp; PLMs: ${s.total_plms} &nbsp;|&nbsp; PLMI: ${s.plmi.toFixed(1)} /hour${hrv}</small>`;

  title += ` &nbsp;|&nbsp; <small>@ ${meta.git_hash}</small>`;
  return title;
}

export function buildChartOption(zarr: ZarrData, meta: SidecarMeta): object {
  // Step-compress RR: keep only transition points
  const rrF32 = zarr.rr;
  const tMs = toMs(zarr.t);
  const transitions: number[] = [0];
  for (let i = 1; i < rrF32.length; i++) {
    if (rrF32[i] !== rrF32[i - 1]) transitions.push(i);
  }
  transitions.push(rrF32.length - 1);

  const rrTMs = new Float64Array(transitions.map(i => tMs[i]));
  const rrVals = new Float32Array(transitions.map(i => rrF32[i]));

  // Accel: every 3rd sample (~3.3 Hz)
  const stride = 3;
  const accLen = Math.floor(zarr.accel_mag.length / stride);
  const accTMs = new Float64Array(accLen);
  const accVals = new Float32Array(accLen);
  for (let i = 0; i < accLen; i++) {
    accTMs[i] = tMs[i * stride];
    accVals[i] = zarr.accel_mag[i * stride];
  }

  const hasHrv = zarr.hrv_t.length > 1;
  const hrvTMs = hasHrv ? toMs(zarr.hrv_t) : new Float64Array(0);

  const rawChannels: { name: string; t: Float64Array; v: Float32Array | Uint8Array }[] = [
    { name: 'Accel X', t: tMs, v: zarr.accel_x },
    { name: 'Accel Y', t: tMs, v: zarr.accel_y },
    { name: 'Accel Z', t: tMs, v: zarr.accel_z },
  ];

  const nExtra = rawChannels.length;
  const nRows = 2 + (hasHrv ? 1 : 0) + nExtra;
  const rowH = (85 / nRows).toFixed(1);

  const grids: object[] = [];
  const xAxes: object[] = [];
  const yAxes: object[] = [];
  const series: object[] = [];

  const yNames = ['RR (ms)', 'Accel magnitude'];
  if (hasHrv) yNames.push('HRV RMSSD (ms)');
  rawChannels.forEach(ch => yNames.push(ch.name));

  for (let r = 0; r < nRows; r++) {
    const top = (5 + r * (parseFloat(rowH) + 2)).toFixed(1);
    grids.push({ left: 70, right: 20, top: `${top}%`, height: `${rowH}%` });
    xAxes.push({
      gridIndex: r, type: 'time',
      axisLabel: {
        show: r === nRows - 1,
        formatter: (v: number) => formatTime(v),
        fontSize: 10,
      },
      splitLine: { show: false },
      axisLine: { lineStyle: { color: '#444' } },
    });
    yAxes.push({
      gridIndex: r, type: 'value',
      name: yNames[r], nameLocation: 'middle', nameGap: 55,
      nameTextStyle: { fontSize: 11, color: '#aaa' },
      splitLine: { lineStyle: { color: '#222' } },
      axisLabel: { fontSize: 10 },
    });
  }

  // LM / PLM markArea data on the accel subplot
  const lmMark = meta.lm_events.map(([a, b]) => [
    { xAxis: BASE_MS + a * 1000, itemStyle: { color: 'rgba(0,180,0,0.25)' }, label: { show: false } },
    { xAxis: BASE_MS + b * 1000 },
  ]);
  const plmMark = meta.plm_groups.map(grp => [
    {
      xAxis: BASE_MS + grp[0][0] * 1000,
      itemStyle: { color: 'rgba(220,0,0,0.10)', borderColor: '#dd0000', borderWidth: 1.5 },
      label: { show: true, position: 'insideTopLeft', formatter: 'PLM', fontSize: 9, color: '#dd0000' },
    },
    { xAxis: BASE_MS + grp[grp.length - 1][1] * 1000 },
  ]);

  series.push({
    name: 'RR (ms)', type: 'line', xAxisIndex: 0, yAxisIndex: 0,
    data: zip(rrTMs, rrVals), step: 'end',
    showSymbol: false, animation: false,
    lineStyle: { color: '#ff4444', width: 1 },
    large: true, largeThreshold: 2000,
    sampling: 'lttb',
  });

  series.push({
    name: 'Accel magnitude', type: 'line', xAxisIndex: 1, yAxisIndex: 1,
    data: zip(accTMs, accVals),
    showSymbol: false, animation: false,
    lineStyle: { color: '#4488ff', width: 1 },
    large: true, largeThreshold: 5000,
    sampling: 'lttb',
    markArea: { silent: true, data: [...lmMark, ...plmMark] },
  });

  let si = 2;
  if (hasHrv) {
    series.push({
      name: 'HRV RMSSD (ms)', type: 'line', xAxisIndex: si, yAxisIndex: si,
      data: zip(hrvTMs, zarr.hrv_rmssd),
      showSymbol: false, animation: false,
      lineStyle: { color: '#cc66ff', width: 1.5 },
      large: true, largeThreshold: 2000,
      sampling: 'lttb',
    });
    si++;
  }

  const chColors = ['#00ffcc', '#ffaa00', '#ff66cc'];
  rawChannels.forEach((ch, ci) => {
    series.push({
      name: ch.name, type: 'line', xAxisIndex: si + ci, yAxisIndex: si + ci,
      data: zip(ch.t, ch.v),
      showSymbol: false, animation: false,
      lineStyle: { color: chColors[ci % chColors.length], width: 1 },
      large: true, largeThreshold: 5000,
      sampling: 'lttb',
    });
  });

  const zIdxs = Array.from({ length: nRows }, (_, i) => i);

  return {
    backgroundColor: '#111',
    animation: false,
    tooltip: {
      trigger: 'axis',
      axisPointer: { animation: false, type: 'cross' },
      formatter: (params: { seriesName: string; value: [number, number] }[]) => {
        if (!params.length) return '';
        const ts = formatTime(params[0].value[0]);
        return params.map(p => `${p.seriesName}: ${Number(p.value[1]).toFixed(1)}`).join('<br>') + `<br><b>${ts}</b>`;
      },
    },
    dataZoom: [
      { type: 'inside', xAxisIndex: zIdxs, filterMode: 'none' },
      { type: 'slider',  xAxisIndex: zIdxs, filterMode: 'none', bottom: 5, height: 22 },
    ],
    grid: grids, xAxis: xAxes, yAxis: yAxes, series,
  };
}

export { buildTitle };
