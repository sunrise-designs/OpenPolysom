import * as echarts from 'echarts';
import { loadMeta, loadEvents, loadZarr, httpStore } from './zarr_loader';
import { buildBubbleOption, buildSingleChannelOption, channelCount, brushRangeFromEvent } from './chart';
import type { BrushRange } from './chart';
import { renderShell, NO_RANGE_TEXT } from './shell';
import { buildNarrative } from './narrative';
import { formatElapsed } from './format';
import type { Meta, EventsDoc, ZarrData } from './types';

const DEFAULT_META = 'public/sample/meta.json';
const CONNECT_GROUP = 'protosom-signals';

function baseDirOf(url: string): string {
  return url.substring(0, url.lastIndexOf('/') + 1);
}

/**
 * Wire the Montage rail's toggles: clicking a `.mont[data-idx]` row flips its
 * switch and shows/hides the `.sig-card`(s) it names (comma-separated channel
 * indices, matching `CHANNELS` order in chart.ts). Rows without `data-idx`
 * (the "awaiting device" ones) have no channel to toggle and stay static.
 */
function wireMontageToggles(): void {
  document.querySelectorAll<HTMLElement>('.mont[data-idx]').forEach((mont) => {
    const idxs = (mont.dataset.idx ?? '').split(',').filter((s) => s !== '').map(Number);
    const toggle = mont.querySelector<HTMLElement>('.toggle');
    if (toggle === null || idxs.length === 0) return;

    mont.addEventListener('click', () => {
      const nowOn = !toggle.classList.contains('on');
      toggle.classList.toggle('on', nowOn);
      toggle.classList.toggle('off', !nowOn);
      mont.classList.toggle('muted-row', !nowOn);
      idxs.forEach((i) => {
        const card = document.getElementById(`sig-${String(i)}`)?.closest('.sig-card');
        if (card instanceof HTMLElement) card.style.display = nowOn ? '' : 'none';
      });
    });
  });
}

/**
 * Mobile: open one channel full-screen in forced landscape. Returns an `open`
 * callback the bubbles wire to their tap handler.
 */
function createFullscreen(zarr: ZarrData, events: EventsDoc): (index: number) => void {
  const overlay = document.createElement('div');
  overlay.id = 'chart-fs';
  overlay.innerHTML =
    '<button class="fs-close" type="button" aria-label="Close full screen">✕ Close</button>' +
    '<div class="fs-rotate-hint">Rotate your device for the widest view</div>' +
    '<div id="chart-fs-canvas"></div>';
  document.body.appendChild(overlay);
  const canvas = overlay.querySelector<HTMLElement>('#chart-fs-canvas');
  const closeBtn = overlay.querySelector<HTMLElement>('.fs-close');
  if (canvas === null || closeBtn === null) return () => undefined;

   
  let fsChart: echarts.ECharts | undefined;

  const close = (): void => {
    try { screen.orientation.unlock(); } catch { /* unsupported */ }
    if (document.fullscreenElement !== null) void document.exitFullscreen().catch(() => undefined);
    overlay.classList.remove('open');
    fsChart?.dispose();
    fsChart = undefined;
  };

  closeBtn.addEventListener('click', close);
  document.addEventListener('fullscreenchange', () => {
    if (document.fullscreenElement === null && overlay.classList.contains('open')) close();
  });
  window.addEventListener('resize', () => { fsChart?.resize(); });

  return (index: number): void => {
    overlay.classList.add('open');
    const chart = echarts.init(canvas, null, { renderer: 'canvas' });
    fsChart = chart;
    chart.setOption(buildSingleChannelOption(zarr, events, index));
    void (async (): Promise<void> => {
      try { await overlay.requestFullscreen(); } catch { /* unsupported */ }
      try { await screen.orientation.lock('landscape'); } catch { /* unsupported (e.g. iOS) */ }
      chart.resize();
    })();
  };
}

function rangesEqual(a: BrushRange | undefined, b: BrushRange | undefined): boolean {
  if (a === undefined || b === undefined) return a === b;
  return Math.abs(a[0] - b[0]) < 1e-6 && Math.abs(a[1] - b[1]) < 1e-6;
}

/**
 * Activate brush-drag x-range selection on every chart and mirror whatever is
 * selected on one pane onto all the others, so they always show the same
 * highlighted band. Updates the shared `#sig-range` readout with the
 * selection's duration (HH:MM:SS).
 *
 * `lastRange` is a fixed-point guard, not a re-entrancy flag: mirroring a
 * selection onto another chart makes that chart re-fire its own
 * `brushSelected`, and whether that happens synchronously or on a later tick
 * is an ECharts implementation detail we don't control. Comparing against the
 * last-applied value (rather than a boolean set/cleared around the dispatch
 * calls) is idempotent regardless of that timing, so the cascade always
 * terminates instead of ping-ponging between charts.
 */
function wireBrushSync(charts: readonly echarts.ECharts[]): void {
  const rangeEl = document.getElementById('sig-range');
  let lastRange: BrushRange | undefined = undefined;

  charts.forEach((chart, i) => {
    chart.dispatchAction({ type: 'takeGlobalCursor', key: 'brush', brushOption: { brushType: 'lineX' } });
    chart.on('brushSelected', (params: unknown) => {
      const range: BrushRange | undefined = brushRangeFromEvent(params);
      if (rangesEqual(range, lastRange)) return;
      lastRange = range;
      if (rangeEl !== null) {
        rangeEl.textContent = range === undefined ? NO_RANGE_TEXT : formatElapsed(range[1] - range[0]);
      }
      charts.forEach((other, j) => {
        if (j === i) return;
        other.dispatchAction(range === undefined
          ? { type: 'brush', areas: [] }
          : { type: 'brush', areas: [{ brushType: 'lineX', xAxisIndex: 0, coordRange: range }] });
      });
    });
  });
}

const ZOOM_STEP = 0.15; // fraction of the current visible window to shrink/grow per wheel notch
const MIN_ZOOM_SPAN_PCT = 0.5; // don't let a zoomed window collapse to nothing

/** Current dataZoom [start, end] percentages (0-100) of a chart's x-axis. */
function currentZoomRange(chart: echarts.ECharts): { readonly start: number; readonly end: number } {
  const opt = chart.getOption() as { dataZoom?: readonly { start?: number; end?: number }[] };
  const dz = opt.dataZoom?.[0];
  return { start: dz?.start ?? 0, end: dz?.end ?? 100 };
}

/** Wheel-delta units vary by device/browser; normalize to a pixel-ish scale. */
function wheelDeltaPixels(e: WheelEvent): number {
  if (e.deltaMode === 1) return e.deltaY * 16; // DOM_DELTA_LINE
  if (e.deltaMode === 2) return e.deltaY * window.innerHeight; // DOM_DELTA_PAGE
  return e.deltaY;
}

/**
 * Ctrl+scroll to zoom the x-axis, synced across every chart. This has to
 * fully own wheel-over-chart, not just add Ctrl-gated zoom on top of the
 * page's normal scroll: ECharts/zrender's canvas swallows every wheel event
 * that reaches it — calling `preventDefault` regardless of dataZoom's own
 * config or whether Ctrl is held (confirmed: even with dataZoom's wheel
 * handling entirely off, and even on a chart with no dataZoom/brush at all,
 * plain scroll over the canvas never reaches the page). Once zrender has
 * called `preventDefault`, the browser's native scroll for that event is
 * gone — there's no "let it through" option after the fact. So when Ctrl
 * isn't held, this manually replays the scroll on `window` instead of
 * relying on default browser behavior.
 */
function wireCtrlZoom(charts: readonly echarts.ECharts[]): void {
  charts.forEach((chart) => {
    // Capture phase, so this runs before the event reaches zrender's own
    // listener on the canvas (a descendant of this container) — bubble-phase
    // wouldn't work here, since zrender calls stopPropagation on its own,
    // which would keep this handler from ever firing at all.
    chart.getDom().addEventListener('wheel', (e: WheelEvent) => {
      e.stopPropagation();
      e.preventDefault();
      if (!e.ctrlKey) {
        window.scrollBy(0, wheelDeltaPixels(e));
        return;
      }
      const { start, end } = currentZoomRange(chart);
      const span = end - start;
      const center = start + span / 2;
      const factor = e.deltaY < 0 ? 1 - ZOOM_STEP : 1 + ZOOM_STEP;
      const newSpan = Math.min(100, Math.max(MIN_ZOOM_SPAN_PCT, span * factor));
      const newStart = Math.max(0, center - newSpan / 2);
      const newEnd = Math.min(100, center + newSpan / 2);
      chart.dispatchAction({ type: 'dataZoom', start: newStart, end: newEnd });
    }, { capture: true, passive: false });
  });
}

async function run(app: HTMLElement, metaUrl: string): Promise<void> {
  const meta: Meta = await loadMeta(metaUrl);
  const base = baseDirOf(metaUrl);
  const [events, zarrData] = await Promise.all([
    loadEvents(new URL('events.json', base).href),
    loadZarr(httpStore(new URL(meta.layers.working.path, base).href)),
  ]);

  app.innerHTML = renderShell(meta, buildNarrative(meta.stats));

  // On touch: drop tooltip + inside-zoom (so swipes scroll) and enable tap-to-fullscreen.
  const touch = window.matchMedia('(pointer: coarse), (max-width: 760px)').matches;
  const openFs = touch ? createFullscreen(zarrData, events) : undefined;
  const count = channelCount(zarrData);

  const charts = Array.from({ length: count }, (_, i) => {
    const el = document.getElementById(`sig-${String(i)}`);
    if (el === null) return undefined;
    const c = echarts.init(el, null, { renderer: 'canvas' });
    c.setOption(buildBubbleOption(zarrData, events, i, touch));
    c.group = CONNECT_GROUP;
    if (openFs !== undefined) {
      c.getZr().on('click', () => { openFs(i); });
    }
    return c;
  }).filter((c): c is echarts.ECharts => c !== undefined);

  echarts.connect(CONNECT_GROUP);
  if (!touch) {
    wireBrushSync(charts);
    wireCtrlZoom(charts);
  }
  wireMontageToggles();
  window.addEventListener('resize', () => { charts.forEach((c) => { c.resize(); }); });
}

function main(): void {
  const app = document.getElementById('app');
  if (app === null) return;
  const param = new URLSearchParams(window.location.search).get('meta');
  const metaUrl = new URL(param ?? DEFAULT_META, window.location.href).href;
  run(app, metaUrl).catch((err: unknown) => {
    app.innerHTML = `<pre class="fatal">Error loading recording:\n${String(err)}</pre>`;
    console.error(err);
  });

  if ('serviceWorker' in navigator) {
    window.addEventListener('load', () => { void navigator.serviceWorker.register('sw.js'); });
  }
}

main();
