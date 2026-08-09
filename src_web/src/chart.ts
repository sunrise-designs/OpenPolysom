import type { EChartsOption } from 'echarts';
import type { EventsDoc } from './types';
import type { Series, SignalSource } from './signals';
import { seriesLength, sourceTMax } from './signals';
import { COLORS } from './tokens';
import { formatElapsed } from './format';

/** One resolved signal channel (descriptor + the data found for it). */
interface ChannelDef {
  readonly name: string;
  readonly color: string;
  /** Canonical channel name — the `SignalSource` key and the `events.json` `channels` tag. */
  readonly key: string;
  readonly overlay: boolean;
  readonly series: Series;
}

interface ChannelSrc {
  readonly name: string;
  readonly color: string;
  /**
   * Canonical snake_case channel name (wiki/planning/zarr-schema-spec.md §3.2).
   * Doubles as the `SignalSource` lookup key and, for overlay panes, as the
   * `events.json` `channels` tag whose spans this pane draws.
   */
  readonly key: string;
  readonly overlay?: boolean;
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
//
// Every row is conditional: a channel renders only when the source actually
// carries data for its key, so one table serves both viewer modes without a
// mode branch anywhere below.
//
//  - **batch** (`zarr_loader.ts`'s `toSignalSource`) supplies the derived-layer
//    channels — `accel_mag`/`hrv_rmssd`/`accel1_mag`/`accel_combined_mag` are
//    Python's scored outputs, and `accel1_mag`/`accel_combined_mag` only exist
//    when a second accelerometer was scored (wiki/knowledge/signal-processing.md).
//    `thoracic`/`abdomen`/`flow` are absent from wrist-only logs.
//  - **RT** (`rt_store.ts`) supplies the raw EDF+ channels the device streams —
//    including `ecg` and the `accel1_*` axes, which no derived store carries —
//    and none of the derived ones, which need Python processing to exist.
//
// A channel renders when its source has samples for it *or* the source declared
// it (`Series.declared`) — the source, not this table, decides which channels
// exist. Batch declares the six the derived layer always writes, so an empty
// HRV pane still renders exactly as before; RT declares whatever the device
// listed in `hello`.
//
// Order matters: it is the on-screen pane order. `ecg` and the `accel1_*` axes
// are positioned so that filtering them out (the batch case) leaves the
// existing batch pane order — and therefore shell.ts's hardcoded montage
// `data-idx` values — exactly as they were.
const CHANNELS: readonly ChannelSrc[] = [
  { name: 'RR · ms', color: COLORS.cardiac, key: 'rr' },
  { name: 'ECG · ADC', color: COLORS.cardiac2, key: 'ecg' },
  { name: 'Accel mag', color: COLORS.movement, key: 'accel_mag', overlay: true },
  { name: 'HRV · ms', color: COLORS.hrv, key: 'hrv_rmssd' },
  { name: 'Accel X', color: COLORS.movement, key: 'accel0_x' },
  { name: 'Accel Y', color: COLORS.movement2, key: 'accel0_y' },
  { name: 'Accel Z', color: COLORS.movement3, key: 'accel0_z' },
  { name: 'Accel1 X (leg 2)', color: COLORS.movement, key: 'accel1_x' },
  { name: 'Accel1 Y (leg 2)', color: COLORS.movement2, key: 'accel1_y' },
  { name: 'Accel1 Z (leg 2)', color: COLORS.movement3, key: 'accel1_z' },
  { name: 'Thoracic', color: COLORS.respir, key: 'thoracic' },
  { name: 'Abdomen', color: COLORS.respir, key: 'abdomen' },
  { name: 'Flow', color: COLORS.respir, key: 'flow' },
  { name: 'Accel1 mag (leg 2)', color: COLORS.movement2, key: 'accel1_mag', overlay: true },
  { name: 'Combined LM (bilateral)', color: COLORS.evtPlm, key: 'accel_combined_mag', overlay: true },
];

const channels = (src: SignalSource): readonly ChannelDef[] =>
  CHANNELS
    .map((c) => ({
      name: c.name,
      color: c.color,
      key: c.key,
      overlay: c.overlay === true,
      series: src[c.key] ?? {},
    }))
    .filter((c) => seriesLength(c.series) > 0 || c.series.declared === true);

/** Per-recording descriptor (name + colour) for rendering bubble headers — filtered to channels the source actually has data for. */
export const channelMeta = (src: SignalSource): readonly { readonly name: string; readonly color: string }[] =>
  channels(src).map((c) => ({ name: c.name, color: c.color }));

/**
 * A channel's rendered position, by name — the montage rail (shell.ts) uses this
 * to wire a toggle's `data-idx` to whichever panes are actually present, instead of
 * assuming a fixed trailing position. Optional channels shift the index of anything
 * after them once filtered, so hardcoding "the last two" (or any other fixed
 * offset) silently breaks the moment two different optional channel groups are
 * present in the same recording (e.g. Accel1 + respiratory, as of this fixture).
 * -1 when the channel isn't present in this recording.
 */
export const channelIndex = (src: SignalSource, name: string): number =>
  channels(src).findIndex((c) => c.name === name);

const zip = (x: Float64Array, y: ArrayLike<number>): readonly (readonly [number, number])[] => {
  const n = Math.min(x.length, y.length);
  return Array.from({ length: n }, (_, i): readonly [number, number] => [x[i] ?? 0, y[i] ?? 0]);
};

/**
 * The `series.data` payload for one channel, plus the `dimensions` ECharts
 * requires alongside it.
 *
 * A live channel already holds its points interleaved (`rt_store.ts`), so it is
 * handed straight over as a typed array. ECharts has a bulk ingest path for
 * that shape (`fillStorage` in `data/helper/dataProvider.js`) which reads the
 * buffer directly and computes the axis extent in the same pass — no per-point
 * JS allocation, which is what makes an unbounded live session affordable.
 * `dimensions` is mandatory for a typed array: it is the only thing telling
 * ECharts the buffer is 2 values per point rather than 1.
 *
 * A batch channel keeps the `[t, v]` pair array it has always used — the whole
 * recording is loaded once, so the allocation is paid once.
 *
 * Note this is deliberately *not* `chart.appendData()`, the other streaming
 * API: it cannot grow a coordinate system's extent (see the note at
 * `echarts/lib/core/echarts.js:982`), and a live x-axis does nothing but grow.
 * `main.ts`'s render loop instead re-sets the whole buffer on a tick whose
 * period scales with the buffer size.
 */
const seriesData = (s: Series): { readonly data: number[][]; readonly dimensions?: string[] } =>
  s.points !== undefined
    ? { data: s.points as unknown as number[][], dimensions: ['t', 'v'] }
    : { data: zip(s.x ?? new Float64Array(0), s.y ?? []) as unknown as number[][] };

// An event/group with no `channels` tag predates the multi-accelerometer schema
// (e.g. tools/make_fixture.py's legacy single-accelerometer sample) — treat it as
// belonging to the one channel that has always implicitly meant "the" accelerometer.
const matchesChannel = (eventChannels: readonly string[] | undefined, channelKey: string): boolean =>
  eventChannels === undefined ? channelKey === 'accel_mag' : eventChannels.includes(channelKey);

const collectSpans = (events: EventsDoc, type: 'limb_movement' | 'plm_series', channelKey: string): readonly Span[] => {
  if (type === 'limb_movement') {
    return events.scorings.flatMap((s) =>
      s.events
        .filter((e) => e.type === 'limb_movement' && matchesChannel(e.channels, channelKey))
        .map((e): Span => ({ start: e.onset_s, end: e.onset_s + e.duration_s })),
    );
  }
  return events.scorings.flatMap((s) =>
    s.groups
      .filter((g) => g.type === 'plm_series' && matchesChannel(g.channels, channelKey))
      .map((g): Span => ({ start: g.onset_s, end: g.onset_s + g.duration_s })),
  );
};

/** LM/PLM markArea spans for one pane, filtered to the events/groups tagged with `channelKey` — so Accel0, Accel1, and the combined pane each only show their own scoring. */
const overlayMarkArea = (events: EventsDoc, channelKey: string): unknown[] => {
  const lm = collectSpans(events, 'limb_movement', channelKey).map((s) => [
    { xAxis: s.start, itemStyle: { color: 'rgba(79,214,163,0.16)', borderColor: COLORS.evtLm, borderWidth: 1, opacity: 0.5 } },
    { xAxis: s.end },
  ]);
  const plm = collectSpans(events, 'plm_series', channelKey).map((s) => [
    {
      xAxis: s.start,
      itemStyle: { color: 'rgba(95,208,196,0.10)', borderColor: COLORS.evtPlm, borderWidth: 1.3, borderType: 'dashed' as const },
      label: { show: true, position: 'insideTopLeft' as const, formatter: 'PLM', fontSize: 9, color: COLORS.evtPlm },
    },
    { xAxis: s.end },
  ]);
  return [...lm, ...plm];
};

// Floored at 1 s so a source with no samples yet (a live stream whose panes are
// built from `hello` before the first frame lands) still gets a non-degenerate
// x-axis. Real recordings are always longer, so batch is unaffected.
const tMaxOf = (src: SignalSource): number => Math.max(sourceTMax(src), 1);

// Decimation is now fully native: each series carries the whole recording once, and
// ECharts' dataZoom (filterMode 'weakFilter') windows it on zoom while `sampling: 'lttb'`
// re-thins whatever's in that window to plotting-area pixel width. Zoomed out → thinned to
// ~pixels (cheap); zoomed in → the window holds fewer samples than pixels, so LTTB thins
// nothing and every real point renders — "the data the chart needs" is the visible window
// (see wiki/knowledge/concepts.md's Decimation entry), computed in ECharts' own typed-array
// loops rather than rebuilt in JS on every zoom event.

export function channelCount(src: SignalSource): number {
  return channels(src).length;
}

/** Stacked view only: which row a vertical pixel falls in; -1 if outside the rows. */
export function channelIndexAtY(yPx: number, heightPx: number, count: number): number {
  if (heightPx <= 0 || count <= 0) return -1;
  const rowH = (100 - TOP_PCT - BOTTOM_PCT - GAP_PCT * (count - 1)) / count;
  const yPct = (yPx / heightPx) * 100;
  const tops = Array.from({ length: count }, (_, r) => TOP_PCT + r * (rowH + GAP_PCT));
  return tops.findIndex((top) => yPct >= top && yPct <= top + rowH);
}

/**
 * The explicit x-axis scroll handle, in one place so every pane's looks and behaves
 * identically. `extra` layers on per-view layout (height/placement/target axes).
 *
 * Declaring the whole `dataZoom` array the same way on every connected chart is
 * load-bearing, not just tidiness: `echarts.connect` syncs by re-dispatching the
 * source chart's event, and a slider drag carries `dataZoomId` (SliderZoomView) —
 * which the receiving chart resolves via `findEffectedDataZooms`' `query: payload`.
 * Auto-generated component ids are `'\0<name>\0<n>'` derived from the component's
 * index within its type (util/model.js `makeIdAndName`), so they only line up across
 * instances while every chart declares its dataZooms in the same order. Reorder them
 * on one chart and that chart silently stops following the others.
 */
const xScrollSlider = (extra: Readonly<Record<string, unknown>> = {}): Record<string, unknown> => ({
  type: 'slider',
  filterMode: 'weakFilter',
  backgroundColor: COLORS.bg,
  borderColor: COLORS.ring,
  fillerColor: 'rgba(95,208,196,0.14)',
  handleStyle: { color: COLORS.movement },
  labelFormatter: (v: number) => formatElapsed(v),
  ...extra,
});

// Single-band x-range brush, shared shape for every bubble chart so the
// selection main.ts mirrors across panes looks identical on each of them.
const brushOption: Record<string, unknown> = {
  xAxisIndex: 0,
  brushType: 'lineX',
  brushMode: 'single',
  removeOnClick: true,
  throttleType: 'debounce',
  throttleDelay: 200,
  brushStyle: { color: 'rgba(95,208,196,0.16)', borderColor: COLORS.movement, borderWidth: 1 },
  toolbox: [],
};

// Selection is auto-activated from main.ts (no toolbox button needed). ECharts'
// brush preprocessor auto-injects a default toolbox (rect/polygon/keep/clear
// icons) whenever `brush.toolbox` resolves empty on first render — an explicit
// `show: false` is the only thing that actually suppresses it.
const hiddenToolboxOption: Record<string, unknown> = { show: false };

/** Range of a brush selection, in elapsed seconds — [start, end], start <= end. */
export type BrushRange = readonly [number, number];

/** Pull the selected [start, end] out of an ECharts `brushSelected` event payload. */
export function brushRangeFromEvent(params: unknown): BrushRange | undefined {
  const batch = (params as { batch?: readonly { areas?: readonly { coordRange?: readonly [number, number] }[] }[] }).batch;
  const range = batch?.[0]?.areas?.[0]?.coordRange;
  return range === undefined ? undefined : [Math.min(range[0], range[1]), Math.max(range[0], range[1])];
}

/** Labels of the pane context menu's items (see main.ts's `wireContextMenu`). */
export const ZOOM_TO_WINDOW_LABEL = 'Zoom to window';
/** Touch-only: arms the brush so the next drag draws a selection. Desktop has drag-to-select already. */
export const SELECT_WINDOW_LABEL = 'Select a window';

/**
 * Floor on the window `zoomToRangeAction` produces, in elapsed seconds. A drag of
 * a few pixels is a legitimate selection (`brushOption.removeOnClick` only clears
 * a true zero-movement click), but zooming to it would collapse the axis to a
 * span ECharts renders as an empty pane with no way back except the slider.
 */
const MIN_ZOOM_SPAN_S = 1;

/** The ECharts action that zooms a pane to a brush selection. Pure — main.ts dispatches it. */
export interface ZoomAction {
  readonly type: 'dataZoom';
  readonly startValue: number;
  readonly endValue: number;
}

/**
 * Zoom every connected pane to `range`, the band the user brush-selected.
 *
 * Expressed in **axis values** (elapsed seconds) rather than the 0–100 `start`/`end`
 * percentages `wireCtrlZoom` uses, so it never has to re-derive `tMax` — and stays
 * correct if a pane's x-axis `max` ever stops being the whole recording. Sub-second
 * selections are widened about their midpoint rather than rejected, so the menu item
 * always does something visible.
 */
export function zoomToRangeAction(range: BrushRange): ZoomAction {
  const mid = (range[0] + range[1]) / 2;
  const half = Math.max(MIN_ZOOM_SPAN_S, range[1] - range[0]) / 2;
  return { type: 'dataZoom', startValue: mid - half, endValue: mid + half };
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
export function buildBubbleOption(src: SignalSource, events: EventsDoc, index: number, touch = false): EChartsOption {
  const rows = channels(src);
  const ch = rows[index];
  if (ch === undefined) return {};
  const tMax = tMaxOf(src);

  const series = {
    name: ch.name,
    type: 'line' as const,
    ...seriesData(ch.series),
    showSymbol: false,
    animation: false,
    lineStyle: { color: ch.color, width: 1.1 },
    large: true,
    largeThreshold: 2000,
    sampling: 'lttb' as const,
    ...(ch.overlay
      ? { areaStyle: { color: 'rgba(95,208,196,0.10)' }, markArea: { silent: true, data: overlayMarkArea(events, ch.key) as unknown as never } }
      : {}),
  };

  return {
    backgroundColor: 'transparent',
    animation: false,
    textStyle: { color: COLORS.textMut, fontFamily: 'Inter, system-ui, sans-serif' },
    // `bottom` clears the scroll handle below (bottom 4 + height 18) plus the time axis' labels.
    grid: { left: 38, right: 8, top: 8, bottom: 36 },
    tooltip: {
      ...sharedTooltip(touch),
      formatter: (params: unknown) => {
        const arr = params as readonly { value?: readonly [number, number] }[];
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
    // Desktop: keep the inside-zoom component (connect-synced) so
    // dispatchAction({type:'dataZoom'}) works, but leave its own wheel/drag
    // handling off — main.ts (wireCtrlZoom) owns the wheel event instead,
    // since zrender's canvas swallows wheel events outright regardless of
    // this config, and only a manual handler can replay the scroll for the
    // non-Ctrl case. See wireCtrlZoom's comment for the full story.
    //
    // The slider is the explicit x-axis scroll handle, on every pane (including
    // touch, which has no inside-zoom to pan with). Per-pane rather than one shared
    // bar because the panes stack vertically at 300px each — a single handle would
    // sit off-screen for most of the page. echarts.connect() keeps them in lockstep;
    // see xScrollSlider on why the array's shape must match across charts.
    //
    // filterMode 'weakFilter': on zoom, ECharts windows the series to the visible range
    // (keeping the points just outside so the line still reaches both edges), then
    // `sampling: 'lttb'` re-thins that window — native adaptive decimation, no per-event
    // JS rebuild. Top-level `animation: false` (above) keeps the y-axis rescale that
    // 'weakFilter' triggers instant, so the line doesn't morph/tween mid-zoom.
    dataZoom: [
      ...(touch ? [] : [{ type: 'inside', filterMode: 'weakFilter', zoomOnMouseWheel: false, moveOnMouseWheel: false, moveOnMouseMove: false }]),
      // showDetail off: the time axis directly above already relabels to the zoomed
      // range, so the drag tooltip is redundant clutter in a pane this compact.
      xScrollSlider({ bottom: 4, height: 18, showDetail: false, textStyle: { color: COLORS.textDim, fontSize: 9 } }),
    ],
    // Drag-to-select an x-range; main.ts mirrors the selection across every pane.
    // Declared on touch too, but *armed* differently: on desktop main.ts turns the
    // brush cursor on at init, whereas on touch that would make every vertical
    // page-scroll swipe over a pane draw a band instead of scrolling (the same
    // canvas-swallows-the-gesture problem the dataZoom rule above describes).
    // There the brush is armed on demand, from the long-press menu — see
    // `wireContextMenu` / `armBrush` in main.ts.
    brush: brushOption,
    toolbox: hiddenToolboxOption,
    series: [series],
  };
}

// ── live (RT) updates ────────────────────────────────────────────────────────

/**
 * An `events.json` with nothing in it, for the live path. RT runs ahead of
 * Python processing, so there is no scoring to overlay — and none of the three
 * `overlay` channels above (all derived) exist in a live source anyway, so this
 * is belt-and-braces rather than load-bearing.
 */
export const NO_EVENTS: EventsDoc = {
  schema: 'protosom.events',
  schema_version: '1.0.0',
  recording_id: '',
  scorings: [],
};

/** Visible span, in seconds, while a live stream is following the newest samples. */
export const LIVE_WINDOW_S = 60;

/**
 * The partial option a live render tick applies to one pane: the channel's
 * current buffer, and the axis extent grown to cover it. Merged into the
 * existing option (ECharts merges `series` by index), so everything else the
 * pane was built with — tooltip, dataZoom, brush, styling — survives untouched.
 */
export function liveUpdateOption(src: SignalSource, index: number, tMax: number): EChartsOption {
  const rows = channels(src);
  if (index < 0 || index >= rows.length) return {};
  const ch = rows[index];
  return {
    xAxis: { max: tMax },
    series: [{ type: 'line', ...seriesData(ch.series) }],
  };
}

/**
 * Keeps the visible window pinned to the newest samples.
 *
 * Expressed as a `dataZoom` action rather than by clamping `xAxis.min` so the
 * *whole* session stays on the axis and reachable with the scroll handle — the
 * user chose to keep everything since connect, and an axis that only spans the
 * last minute would throw that away. `echarts.connect` mirrors the single
 * dispatch across every pane, exactly as `zoomToRangeAction` relies on.
 */
export function followLiveAction(tMax: number, windowS = LIVE_WINDOW_S): ZoomAction {
  return { type: 'dataZoom', startValue: Math.max(0, tMax - windowS), endValue: tMax };
}

/**
 * One channel filling the whole canvas — mobile full-screen (landscape) view.
 */
export function buildSingleChannelOption(src: SignalSource, events: EventsDoc, index: number): EChartsOption {
  const rows = channels(src);
  const ch = rows[index];
  if (ch === undefined) return {};
  const tMax = tMaxOf(src);

  const oneSeries = {
    name: ch.name,
    type: 'line' as const,
    ...seriesData(ch.series),
    showSymbol: false,
    animation: false,
    lineStyle: { color: ch.color, width: 1.2 },
    large: true,
    largeThreshold: 2000,
    sampling: 'lttb' as const,
    ...(ch.overlay
      ? { areaStyle: { color: 'rgba(95,208,196,0.10)' }, markArea: { silent: true, data: overlayMarkArea(events, ch.key) as unknown as never } }
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
        const arr = params as readonly { value?: readonly [number, number] }[];
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
      { type: 'inside', filterMode: 'weakFilter' },
      xScrollSlider({ bottom: 14, height: 24, textStyle: { color: COLORS.textDim } }),
    ],
    series: [oneSeries],
  };
}

/**
 * Legacy stacked multi-grid view (kept for easy revert from the bubble layout).
 */
export function buildChartOption(src: SignalSource, events: EventsDoc, touch = false): EChartsOption {
  const rows = channels(src);
  const nRows = rows.length;
  const tMax = tMaxOf(src);
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
  const series = rows.map((ch, r) => ({
    name: ch.name, type: 'line' as const, xAxisIndex: r, yAxisIndex: r,
    ...seriesData(ch.series),
    showSymbol: false, animation: false, lineStyle: { color: ch.color, width: 1.1 },
    large: true, largeThreshold: 2000, sampling: 'lttb' as const,
    ...(ch.overlay ? { areaStyle: { color: 'rgba(95,208,196,0.10)' }, markArea: { silent: true, data: overlayMarkArea(events, ch.key) as unknown as never } } : {}),
  }));
  const allGridIdx = rows.map((_, i) => i);

  return {
    backgroundColor: 'transparent', animation: false,
    textStyle: { color: COLORS.textMut, fontFamily: 'Inter, system-ui, sans-serif' },
    tooltip: {
      ...sharedTooltip(touch),
      formatter: (params: unknown) => {
        const arr = params as readonly { seriesName?: string; value?: readonly [number, number] }[];
        const first = arr[0];
        if (first?.value === undefined) return '';
        const lines = arr.filter((p) => p.value !== undefined).map((p) => `${p.seriesName ?? ''}: ${(p.value?.[1] ?? 0).toFixed(1)}`);
        return [...lines, `<b>${formatElapsed(first.value[0])}</b>`].join('<br>');
      },
    },
    dataZoom: [
      ...(touch ? [] : [{ type: 'inside' as const, xAxisIndex: allGridIdx, filterMode: 'weakFilter' as const }]),
      xScrollSlider({ xAxisIndex: allGridIdx, bottom: 6, height: 20, textStyle: { color: COLORS.textDim, fontSize: 10 } }),
    ],
    grid, xAxis, yAxis, series,
  };
}
