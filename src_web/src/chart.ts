import type { EChartsOption } from 'echarts';
import type { ZarrData, EventsDoc } from './types';
import { COLORS } from './tokens';
import { formatElapsed } from './format';

/** One resolved signal channel (descriptor + data). */
interface ChannelDef {
  readonly name: string;
  readonly color: string;
  readonly x: Float64Array;
  readonly y: ArrayLike<number>;
  readonly overlay: boolean;
}

interface ChannelSrc {
  readonly name: string;
  readonly color: string;
  readonly pick: (z: ZarrData) => { readonly x: Float64Array; readonly y: ArrayLike<number>; readonly overlay: boolean };
}

interface Span {
  readonly start: number;
  readonly end: number;
}

// Stacked-layout constants (kept for the legacy stacked view + click hit-test).
const TOP_PCT = 2.5;
const BOTTOM_PCT = 8.5;
const GAP_PCT = 1.0;

// Single source of truth for the channel order, labels, colours, and data binding.
const CHANNELS: ReadonlyArray<ChannelSrc> = [
  { name: 'RR · ms', color: COLORS.cardiac, pick: (z) => ({ x: z.t, y: z.rr, overlay: false }) },
  { name: 'Accel mag', color: COLORS.movement, pick: (z) => ({ x: z.t, y: z.accel_mag, overlay: true }) },
  { name: 'HRV · ms', color: COLORS.hrv, pick: (z) => ({ x: z.hrv_t, y: z.hrv_rmssd, overlay: false }) },
  { name: 'Accel X', color: COLORS.movement, pick: (z) => ({ x: z.t, y: z.accel_x, overlay: false }) },
  { name: 'Accel Y', color: COLORS.movement2, pick: (z) => ({ x: z.t, y: z.accel_y, overlay: false }) },
  { name: 'Accel Z', color: COLORS.movement3, pick: (z) => ({ x: z.t, y: z.accel_z, overlay: false }) },
];

/** Static descriptor (name + colour) for rendering bubble headers without data. */
export const CHANNEL_META: ReadonlyArray<{ readonly name: string; readonly color: string }> =
  CHANNELS.map((c) => ({ name: c.name, color: c.color }));

const channels = (zarr: ZarrData): ReadonlyArray<ChannelDef> =>
  CHANNELS.map((c) => ({ name: c.name, color: c.color, ...c.pick(zarr) }));

const zipStrided = (x: Float64Array, y: ArrayLike<number>, stride: number): ReadonlyArray<readonly [number, number]> => {
  const n = Math.min(x.length, y.length);
  const count = Math.floor(n / stride);
  return Array.from({ length: count }, (_, i): readonly [number, number] => [x[i * stride] ?? 0, y[i * stride] ?? 0]);
};

const collectSpans = (events: EventsDoc, type: 'limb_movement' | 'plm_series'): ReadonlyArray<Span> => {
  if (type === 'limb_movement') {
    return events.scorings.flatMap((s) =>
      s.events.filter((e) => e.type === 'limb_movement').map((e): Span => ({ start: e.onset_s, end: e.onset_s + e.duration_s })),
    );
  }
  return events.scorings.flatMap((s) =>
    s.groups.filter((g) => g.type === 'plm_series').map((g): Span => ({ start: g.onset_s, end: g.onset_s + g.duration_s })),
  );
};

const overlayMarkArea = (events: EventsDoc): unknown[] => {
  const lm = collectSpans(events, 'limb_movement').map((s) => [
    { xAxis: s.start, itemStyle: { color: 'rgba(79,214,163,0.16)', borderColor: COLORS.evtLm, borderWidth: 1, opacity: 0.5 } },
    { xAxis: s.end },
  ]);
  const plm = collectSpans(events, 'plm_series').map((s) => [
    {
      xAxis: s.start,
      itemStyle: { color: 'rgba(95,208,196,0.10)', borderColor: COLORS.evtPlm, borderWidth: 1.3, borderType: 'dashed' as const },
      label: { show: true, position: 'insideTopLeft' as const, formatter: 'PLM', fontSize: 9, color: COLORS.evtPlm },
    },
    { xAxis: s.end },
  ]);
  return [...lm, ...plm];
};

const tMaxOf = (zarr: ZarrData): number => (zarr.t.length > 0 ? (zarr.t[zarr.t.length - 1] ?? 0) : 0);
const strideFor = (name: string): number => (name.startsWith('Accel ') && name !== 'Accel mag' ? 3 : 1);

export function channelCount(zarr: ZarrData): number {
  return channels(zarr).length;
}

/** Stacked view only: which row a vertical pixel falls in; -1 if outside the rows. */
export function channelIndexAtY(yPx: number, heightPx: number, count: number): number {
  if (heightPx <= 0 || count <= 0) return -1;
  const rowH = (100 - TOP_PCT - BOTTOM_PCT - GAP_PCT * (count - 1)) / count;
  const yPct = (yPx / heightPx) * 100;
  const tops = Array.from({ length: count }, (_, r) => TOP_PCT + r * (rowH + GAP_PCT));
  return tops.findIndex((top) => yPct >= top && yPct <= top + rowH);
}

const sharedTooltip = (touch: boolean): Record<string, unknown> =>
  touch
    ? { show: false }
    : {
        trigger: 'axis',
        axisPointer: { animation: false, type: 'cross', lineStyle: { color: COLORS.movement } },
        backgroundColor: COLORS.surface2,
        borderColor: COLORS.ring,
        textStyle: { color: COLORS.text, fontSize: 11 },
      };

/**
 * One channel in its own card ("bubble") — compact, with y-axis + time axis.
 * Pure. On touch the tooltip + inside-zoom are dropped so swipes scroll; charts
 * share a group so echarts.connect() keeps them zoom/cursor-synced.
 */
export function buildBubbleOption(zarr: ZarrData, events: EventsDoc, index: number, touch = false): EChartsOption {
  const rows = channels(zarr);
  const ch = rows[index];
  if (ch === undefined) return {};
  const tMax = tMaxOf(zarr);

  const series = {
    name: ch.name,
    type: 'line' as const,
    data: zipStrided(ch.x, ch.y, strideFor(ch.name)) as unknown as number[][],
    showSymbol: false,
    animation: false,
    lineStyle: { color: ch.color, width: 1.1 },
    large: true,
    largeThreshold: 2000,
    sampling: 'lttb' as const,
    ...(ch.overlay
      ? { areaStyle: { color: 'rgba(95,208,196,0.10)' }, markArea: { silent: true, data: overlayMarkArea(events) as unknown as never } }
      : {}),
  };

  return {
    backgroundColor: 'transparent',
    animation: false,
    textStyle: { color: COLORS.textMut, fontFamily: 'Inter, system-ui, sans-serif' },
    grid: { left: 50, right: 14, top: 8, bottom: 22 },
    tooltip: {
      ...sharedTooltip(touch),
      formatter: (params: unknown) => {
        const arr = params as ReadonlyArray<{ value?: readonly [number, number] }>;
        const first = arr[0];
        if (first?.value === undefined) return '';
        return `${ch.name}: ${first.value[1].toFixed(1)}<br><b>${formatElapsed(first.value[0])}</b>`;
      },
    },
    xAxis: {
      type: 'value',
      min: 0,
      max: tMax,
      axisLabel: { formatter: (v: number) => formatElapsed(v), fontSize: 9, color: COLORS.textDim },
      axisLine: { lineStyle: { color: COLORS.grid } },
      axisTick: { show: false },
      splitLine: { show: false },
    },
    yAxis: {
      type: 'value',
      scale: true,
      splitNumber: 2,
      axisLabel: { fontSize: 9, color: COLORS.textDim, hideOverlap: true },
      axisLine: { show: false },
      axisTick: { show: false },
      splitLine: { lineStyle: { color: COLORS.grid, opacity: 0.6 } },
    },
    dataZoom: touch ? [] : [{ type: 'inside', filterMode: 'none' }],
    series: [series],
  };
}

/**
 * One channel filling the whole canvas — mobile full-screen (landscape) view.
 */
export function buildSingleChannelOption(zarr: ZarrData, events: EventsDoc, index: number): EChartsOption {
  const rows = channels(zarr);
  const ch = rows[index];
  if (ch === undefined) return {};
  const tMax = tMaxOf(zarr);

  const oneSeries = {
    name: ch.name,
    type: 'line' as const,
    data: zipStrided(ch.x, ch.y, 1) as unknown as number[][],
    showSymbol: false,
    animation: false,
    lineStyle: { color: ch.color, width: 1.2 },
    large: true,
    largeThreshold: 2000,
    sampling: 'lttb' as const,
    ...(ch.overlay
      ? { areaStyle: { color: 'rgba(95,208,196,0.10)' }, markArea: { silent: true, data: overlayMarkArea(events) as unknown as never } }
      : {}),
  };

  return {
    backgroundColor: 'transparent',
    animation: false,
    textStyle: { color: COLORS.textMut, fontFamily: 'Inter, system-ui, sans-serif' },
    title: { text: ch.name, left: 16, top: 10, textStyle: { color: ch.color, fontSize: 14, fontWeight: 600 } },
    grid: { left: 64, right: 28, top: 48, bottom: 70 },
    tooltip: {
      trigger: 'axis',
      axisPointer: { animation: false, type: 'cross', lineStyle: { color: COLORS.movement } },
      backgroundColor: COLORS.surface2,
      borderColor: COLORS.ring,
      textStyle: { color: COLORS.text, fontSize: 12 },
      formatter: (params: unknown) => {
        const arr = params as ReadonlyArray<{ value?: readonly [number, number] }>;
        const first = arr[0];
        if (first?.value === undefined) return '';
        return `${ch.name}: ${first.value[1].toFixed(1)}<br><b>${formatElapsed(first.value[0])}</b>`;
      },
    },
    xAxis: {
      type: 'value',
      min: 0,
      max: tMax,
      axisLabel: { formatter: (v: number) => formatElapsed(v), fontSize: 11, color: COLORS.textDim },
      axisLine: { lineStyle: { color: COLORS.grid } },
      splitLine: { show: false },
    },
    yAxis: {
      type: 'value',
      scale: true,
      axisLabel: { fontSize: 11, color: COLORS.textDim },
      axisLine: { show: false },
      axisTick: { show: false },
      splitLine: { lineStyle: { color: COLORS.grid, opacity: 0.6 } },
    },
    dataZoom: [
      { type: 'inside', filterMode: 'none' },
      { type: 'slider', filterMode: 'none', bottom: 14, height: 24, backgroundColor: COLORS.bg, borderColor: COLORS.ring, fillerColor: 'rgba(95,208,196,0.14)', handleStyle: { color: COLORS.movement }, textStyle: { color: COLORS.textDim }, labelFormatter: (v: number) => formatElapsed(v) },
    ],
    series: [oneSeries],
  };
}

/**
 * Legacy stacked multi-grid view (kept for easy revert from the bubble layout).
 */
export function buildChartOption(zarr: ZarrData, events: EventsDoc, touch = false): EChartsOption {
  const rows = channels(zarr);
  const nRows = rows.length;
  const tMax = tMaxOf(zarr);
  const rowH = (100 - TOP_PCT - BOTTOM_PCT - GAP_PCT * (nRows - 1)) / nRows;

  const grid = rows.map((_, r) => ({ left: 62, right: 14, top: `${String(TOP_PCT + r * (rowH + GAP_PCT))}%`, height: `${String(rowH)}%` }));
  const xAxis = rows.map((_, r) => ({
    gridIndex: r, type: 'value' as const, min: 0, max: tMax,
    axisLabel: { show: r === nRows - 1, formatter: (v: number) => formatElapsed(v), fontSize: 10, color: COLORS.textDim },
    axisLine: { lineStyle: { color: COLORS.grid } }, axisTick: { show: false }, splitLine: { show: false },
  }));
  const yAxis = rows.map((ch, r) => ({
    gridIndex: r, type: 'value' as const, scale: true, splitNumber: 2, name: ch.name,
    nameLocation: 'middle' as const, nameRotate: 90, nameGap: 46,
    nameTextStyle: { fontSize: 10.5, color: ch.color, fontWeight: 600 as const },
    axisLabel: { show: true, fontSize: 9, color: COLORS.textDim, hideOverlap: true, showMaxLabel: false },
    axisLine: { show: false }, axisTick: { show: false }, splitLine: { lineStyle: { color: COLORS.grid, opacity: 0.6 } },
  }));
  const mark = overlayMarkArea(events);
  const series = rows.map((ch, r) => ({
    name: ch.name, type: 'line' as const, xAxisIndex: r, yAxisIndex: r,
    data: zipStrided(ch.x, ch.y, strideFor(ch.name)) as unknown as number[][],
    showSymbol: false, animation: false, lineStyle: { color: ch.color, width: 1.1 },
    large: true, largeThreshold: 2000, sampling: 'lttb' as const,
    ...(ch.overlay ? { areaStyle: { color: 'rgba(95,208,196,0.10)' }, markArea: { silent: true, data: mark as unknown as never } } : {}),
  }));
  const allGridIdx = rows.map((_, i) => i);

  return {
    backgroundColor: 'transparent', animation: false,
    textStyle: { color: COLORS.textMut, fontFamily: 'Inter, system-ui, sans-serif' },
    tooltip: {
      ...sharedTooltip(touch),
      formatter: (params: unknown) => {
        const arr = params as ReadonlyArray<{ seriesName?: string; value?: readonly [number, number] }>;
        const first = arr[0];
        if (first === undefined || first.value === undefined) return '';
        const lines = arr.filter((p) => p.value !== undefined).map((p) => `${p.seriesName ?? ''}: ${(p.value?.[1] ?? 0).toFixed(1)}`);
        return [...lines, `<b>${formatElapsed(first.value[0])}</b>`].join('<br>');
      },
    },
    dataZoom: [
      ...(touch ? [] : [{ type: 'inside' as const, xAxisIndex: allGridIdx, filterMode: 'none' as const }]),
      { type: 'slider', xAxisIndex: allGridIdx, filterMode: 'none', bottom: 6, height: 20, backgroundColor: COLORS.bg, borderColor: COLORS.ring, fillerColor: 'rgba(95,208,196,0.14)', handleStyle: { color: COLORS.movement }, textStyle: { color: COLORS.textDim, fontSize: 10 }, labelFormatter: (v: number) => formatElapsed(v) },
    ],
    grid, xAxis, yAxis, series,
  };
}
