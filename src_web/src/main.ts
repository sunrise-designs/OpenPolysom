import * as echarts from 'echarts';
import { loadMeta, loadEvents, loadZarr, loadStudies, httpStore, toSignalSource } from './zarr_loader';
import {
  buildBubbleOption, buildSingleChannelOption, channelCount, brushRangeFromEvent,
  zoomToRangeAction, followLiveAction, liveUpdateOption, ZOOM_TO_WINDOW_LABEL, SELECT_WINDOW_LABEL,
  NO_EVENTS,
} from './chart';
import type { BrushRange } from './chart';
import { renderShell, renderRealtimeShell, renderRtStatus, NO_RANGE_TEXT, renderWindowMetricsBody } from './shell';
import type { RtStatusView } from './shell';
import { renderLanding } from './landing';
import { buildNarrative } from './narrative';
import { formatElapsed } from './format';
import { metricsBaseUrl } from './metrics_config';
import { rtWsUrl } from './rt_config';
import { connectRt } from './rt_client';
import { RtStore } from './rt_store';
import { requestWindowedMetrics } from './metrics_client';
import type { RtHello, RtLinkState, RtStatus } from './rt_types';
import type { SignalSource } from './signals';
import type { Meta, EventsDoc, MetricRequestItem } from './types';

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
 *
 * Takes a getter rather than the source itself so live mode opens on the
 * current buffer instead of whatever had arrived when the panes were built.
 */
function createFullscreen(getSource: () => SignalSource, events: EventsDoc): (index: number) => void {
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
    chart.setOption(buildSingleChannelOption(getSource(), events, index));
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
/**
 * Fire a windowed-PLMI request for a brush selection and render the result
 * into `#window-metrics-body`. `metricsUrl === undefined` means the feature
 * is unconfigured (see `metrics_config.ts`) — the caller hides the whole
 * card in that case, so this is a no-op then. A monotonically increasing
 * request sequence number discards a stale response that resolves after a
 * newer selection has already superseded it (the brush's own 200ms
 * throttle — `chart.ts`'s `brushOption.throttleDelay` — makes overlapping
 * requests a real possibility on a fast drag).
 */
function wireWindowMetrics(recordingId: string, metricsUrl: string | undefined): (range: BrushRange | undefined) => void {
  const bodyEl = document.getElementById('window-metrics-body');
  let requestSeq = 0;

  return (range: BrushRange | undefined): void => {
    if (bodyEl === null || metricsUrl === undefined) return;
    if (range === undefined) {
      bodyEl.innerHTML = renderWindowMetricsBody({ kind: 'empty' });
      return;
    }
    const seq = ++requestSeq;
    bodyEl.innerHTML = renderWindowMetricsBody({ kind: 'computing' });
    const items: readonly MetricRequestItem[] = [{ metric: 'plmi', channel: 'accel_mag' }];
    requestWindowedMetrics(metricsUrl, recordingId, range, items)
      .then((res) => {
        if (seq !== requestSeq) return; // superseded by a newer selection
        if (!res.ok) {
          bodyEl.innerHTML = renderWindowMetricsBody({ kind: 'unavailable', reason: res.reason });
          return;
        }
        if (res.data.results.length === 0) {
          bodyEl.innerHTML = renderWindowMetricsBody({ kind: 'unavailable', reason: 'metrics service returned no results' });
          return;
        }
        const [first] = res.data.results;
        bodyEl.innerHTML = renderWindowMetricsBody({ kind: 'result', result: first });
      })
      .catch(() => {
        if (seq === requestSeq) bodyEl.innerHTML = renderWindowMetricsBody({ kind: 'unavailable', reason: 'metrics service unavailable' });
      });
  };
}

/**
 * Turn the brush cursor on, so the next drag over a pane draws a selection band
 * instead of doing nothing. Split out of `wireBrushSync` because the two input
 * modes arm it at different moments: desktop at init (a drag has no other
 * meaning there), touch only when the long-press menu's "Select a window" is
 * chosen — arming it permanently on touch would turn every vertical page-scroll
 * swipe over a pane into a selection. See `buildBubbleOption`'s `brush` comment.
 */
function armBrush(charts: readonly echarts.ECharts[]): void {
  charts.forEach((chart) => {
    chart.dispatchAction({ type: 'takeGlobalCursor', key: 'brush', brushOption: { brushType: 'lineX' } });
  });
}

function wireBrushSync(charts: readonly echarts.ECharts[], onRangeChange: (range: BrushRange | undefined) => void): () => BrushRange | undefined {
  const rangeEl = document.getElementById('sig-range');
  let lastRange: BrushRange | undefined = undefined;

  charts.forEach((chart, i) => {
    chart.on('brushSelected', (params: unknown) => {
      const range: BrushRange | undefined = brushRangeFromEvent(params);
      if (rangesEqual(range, lastRange)) return;
      lastRange = range;
      if (rangeEl !== null) {
        rangeEl.textContent = range === undefined ? NO_RANGE_TEXT : formatElapsed(range[1] - range[0]);
      }
      onRangeChange(range);
      charts.forEach((other, j) => {
        if (j === i) return;
        other.dispatchAction(range === undefined
          ? { type: 'brush', areas: [] }
          : { type: 'brush', areas: [{ brushType: 'lineX', xAxisIndex: 0, coordRange: range }] });
      });
    });
  });

  return () => lastRange;
}

const LONG_PRESS_MS = 500; // touch equivalent of a right-click; matches the platform convention
const LONG_PRESS_SLOP_PX = 10; // finger drift beyond this is a scroll, not a press — cancel
const TAP_SUPPRESS_MS = 500; // window after a long-press in which the synthetic tap is ignored

/**
 * A right-click (desktop) / long-press (touch) menu on any signal pane, offering
 * "Zoom to window" — zoom every connected pane to the band last brush-selected.
 *
 * A plain DOM menu on `body` rather than an ECharts toolbox button: the bubbles are
 * ~300px tall and a toolbox would eat plotting area on every one of them, and both
 * `contextmenu` and a held touch reach the container before zrender does anything
 * with them.
 *
 * Two input modes, one menu:
 *  - **Desktop** — `contextmenu`. The brush is already armed, so "Zoom to window" is
 *    the only item; it is *disabled* rather than hidden when nothing is selected, so
 *    the menu still advertises the feature to someone who hasn't found the drag.
 *  - **Touch** — a 500 ms press that survives <10px of drift (so scrolling a pane
 *    still just scrolls). There is no always-on brush to select with, so the menu
 *    leads with "Select a window", which arms the brush for the following drag;
 *    "Zoom to window" then applies it.
 *
 * `onOpen` lets the caller swallow the synthetic tap a long-press emits on release,
 * which would otherwise also open the mobile full-screen overlay.
 */
function wireContextMenu(
  charts: readonly echarts.ECharts[],
  selection: () => BrushRange | undefined,
  touch: boolean,
  onOpen: () => void,
): void {
  const menu = document.createElement('div');
  menu.className = 'ctx-menu';
  menu.innerHTML =
    (touch ? `<button type="button" class="ctx-item" data-action="select">${SELECT_WINDOW_LABEL}</button>` : '') +
    `<button type="button" class="ctx-item" data-action="zoom">${ZOOM_TO_WINDOW_LABEL}</button>`;
  document.body.appendChild(menu);

  const zoomItem = menu.querySelector<HTMLButtonElement>('[data-action="zoom"]');
  const selectItem = menu.querySelector<HTMLButtonElement>('[data-action="select"]');
  if (zoomItem === null) return;

  const hide = (): void => { menu.classList.remove('open'); };

  const open = (x: number, y: number): void => {
    zoomItem.disabled = selection() === undefined;
    menu.classList.add('open'); // before measuring, so the edge-flip below sees real dimensions
    const { width, height } = menu.getBoundingClientRect();
    menu.style.left = `${String(Math.max(4, Math.min(x, window.innerWidth - width - 8)))}px`;
    menu.style.top = `${String(Math.max(4, Math.min(y, window.innerHeight - height - 8)))}px`;
    onOpen();
  };

  zoomItem.addEventListener('click', () => {
    const range = selection();
    hide();
    // One dispatch is enough — echarts.connect mirrors dataZoom across the group.
    if (range !== undefined) charts[0]?.dispatchAction(zoomToRangeAction(range));
  });
  selectItem?.addEventListener('click', () => {
    hide();
    armBrush(charts);
  });

  charts.forEach((chart) => {
    const dom = chart.getDom();
    dom.addEventListener('contextmenu', (e: MouseEvent) => {
      e.preventDefault();
      open(e.clientX, e.clientY);
    });

    if (!touch) return;
    let timer: number | undefined;
    let origin: { readonly x: number; readonly y: number } | undefined;
    const cancel = (): void => {
      if (timer !== undefined) window.clearTimeout(timer);
      timer = undefined;
      origin = undefined;
    };

    // `.item(0)` rather than `touches[0]`: TouchList's indexed access is typed
    // non-nullable, so an empty list would read back as a phantom Touch.
    dom.addEventListener('touchstart', (e: TouchEvent) => {
      const t = e.touches.item(0);
      if (t === null || e.touches.length > 1) { cancel(); return; } // a pinch is not a press
      const { clientX, clientY } = t;
      origin = { x: clientX, y: clientY };
      timer = window.setTimeout(() => { open(clientX, clientY); cancel(); }, LONG_PRESS_MS);
    }, { passive: true });

    dom.addEventListener('touchmove', (e: TouchEvent) => {
      const t = e.touches.item(0);
      if (t === null || origin === undefined) return;
      if (Math.abs(t.clientX - origin.x) > LONG_PRESS_SLOP_PX || Math.abs(t.clientY - origin.y) > LONG_PRESS_SLOP_PX) cancel();
    }, { passive: true });

    dom.addEventListener('touchend', cancel, { passive: true });
    dom.addEventListener('touchcancel', cancel, { passive: true });
  });

  document.addEventListener('pointerdown', (e) => {
    if (!(e.target instanceof Node) || !menu.contains(e.target)) hide();
  });
  document.addEventListener('keydown', (e) => { if (e.key === 'Escape') hide(); });
  window.addEventListener('scroll', hide, { passive: true });
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
  const source = toSignalSource(zarrData);

  app.innerHTML = renderShell(meta, buildNarrative(meta.stats), source);

  // Windowed-metrics card: hidden entirely when no metrics service is
  // configured for this deployment (see metrics_config.ts) rather than
  // shown in a permanently-broken state.
  const metricsUrl = metricsBaseUrl(window.location);
  const metricsCard = document.getElementById('window-metrics-card');
  if (metricsUrl === undefined && metricsCard instanceof HTMLElement) {
    metricsCard.style.display = 'none';
  }

  // On touch: drop tooltip + inside-zoom (so swipes scroll) and enable tap-to-fullscreen.
  const touch = window.matchMedia('(pointer: coarse), (max-width: 760px)').matches;
  const openFs = touch ? createFullscreen(() => source, events) : undefined;
  const count = channelCount(source);

  // A long-press fires a synthetic tap on release; without this the mobile
  // full-screen overlay would open behind the menu that press just summoned.
  let menuOpenedAt = 0;

  const charts = Array.from({ length: count }, (_, i) => {
    const el = document.getElementById(`sig-${String(i)}`);
    if (el === null) return undefined;
    const c = echarts.init(el, null, { renderer: 'canvas' });
    c.setOption(buildBubbleOption(source, events, i, touch));
    c.group = CONNECT_GROUP;
    if (openFs !== undefined) {
      c.getZr().on('click', () => {
        if (Date.now() - menuOpenedAt < TAP_SUPPRESS_MS) return;
        openFs(i);
      });
    }
    return c;
  }).filter((c): c is echarts.ECharts => c !== undefined);

  echarts.connect(CONNECT_GROUP);
  const selection = wireBrushSync(charts, wireWindowMetrics(meta.recording_id, metricsUrl));
  // Desktop arms the brush up front; touch arms it from the menu (see wireContextMenu).
  if (!touch) {
    armBrush(charts);
    wireCtrlZoom(charts);
  }
  wireContextMenu(charts, selection, touch, () => { menuOpenedAt = Date.now(); });
  wireMontageToggles();
  window.addEventListener('resize', () => { charts.forEach((c) => { c.resize(); }); });
}

// ── RT (live) mode ───────────────────────────────────────────────────────────

/** Fastest live redraw. 10 Hz looks continuous without out-running the data. */
const RT_MIN_TICK_MS = 100;
/** Slowest live redraw, once a long session has made each redraw expensive. */
const RT_MAX_TICK_MS = 2000;
/** Points a redraw may re-ingest per millisecond of tick period (see `liveTickMs`). */
const RT_POINTS_PER_MS = 2000;

/**
 * How long to wait before the next live redraw.
 *
 * Every tick hands ECharts the whole buffer again, because the alternative
 * (`chart.appendData`) cannot grow a coordinate system's extent and a live axis
 * does nothing but grow. Re-ingest is a bulk typed-array copy, so it is cheap
 * per point — but it is O(total points), and the user chose to keep everything
 * since connect. Scaling the period with the buffer keeps the *fraction* of
 * time spent redrawing roughly constant: 10 Hz for the first few minutes,
 * easing to 0.5 Hz deep into a night, where a second of latency on a trend view
 * costs nothing.
 */
function liveTickMs(points: number): number {
  return Math.min(RT_MAX_TICK_MS, Math.max(RT_MIN_TICK_MS, points / RT_POINTS_PER_MS));
}

/** A dispatch of our own that came back as a `dataZoom` event, within this window, is not user intent. */
const SELF_ZOOM_GUARD_MS = 50;

/**
 * Whether a `hello` continues the stream we were already watching, rather than
 * announcing a new one. A device that merely lost the link comes back with the
 * same recording start and channel table, and its buffers stay valid; one that
 * rebooted into a fresh recording does not, and needs the panes rebuilt.
 */
const isSameStream = (prev: RtHello, next: RtHello): boolean =>
  prev.recording_start_iso === next.recording_start_iso
  && prev.channels.length === next.channels.length;

/**
 * Live mode: plot samples as the device streams them.
 *
 * Structurally identical to `run()` once the panes exist — same
 * `buildBubbleOption`, same `echarts.connect` group, same brush/zoom/context
 * menu wiring. The differences are that the data arrives over time instead of
 * all at once, and that everything downstream of Python processing (scored
 * events, PLMI, HRV, the windowed-metrics card) has nothing to show yet and is
 * absent from the live shell rather than rendered empty.
 */
function runRealtime(app: HTMLElement, url: string): void {
  let store: RtStore | undefined;
  let charts: echarts.ECharts[] = [];
  let hello: RtHello | undefined;
  let status: RtStatus | undefined;
  let link: RtLinkState = 'connecting';
  let linkDetail: string | undefined;
  let following = true;
  let selfZoomAt = 0;
  let renderedPoints = -1;
  let badFrames = (): number => 0;

  const paintStatus = (): void => {
    const el = document.getElementById('rt-status');
    if (el === null || hello === undefined) return;
    const view: RtStatusView = {
      link,
      detail: linkDetail,
      deviceUid: hello.device_uid,
      startIso: hello.recording_start_iso,
      elapsedS: status?.elapsed_s ?? 0,
      recording: status?.recording ?? hello.recording_active,
      sdError: status?.sd_error ?? false,
      battPct: status?.batt_pct ?? null,
      deviceDropped: status?.dropped_frames ?? 0,
      badFrames: badFrames(),
      breaks: store?.breakCount ?? 0,
      points: store?.totalPoints ?? 0,
    };
    el.innerHTML = renderRtStatus(view);
  };

  const setFollowing = (on: boolean): void => {
    following = on;
    const btn = document.getElementById('rt-follow');
    if (btn === null) return;
    btn.classList.toggle('on', on);
    btn.textContent = on ? 'Following live' : 'Jump to live';
  };

  /** (Re)build the panes. Called on the first `hello`, and again if the device starts a new recording. */
  const buildPanes = (h: RtHello, s: RtStore): void => {
    charts.forEach((c) => { c.dispose(); });
    const source = s.source();
    app.innerHTML = renderRealtimeShell(h, source);

    const touch = window.matchMedia('(pointer: coarse), (max-width: 760px)').matches;
    // The source object is rebuilt every tick, so the full-screen view is handed
    // a getter rather than a snapshot — otherwise it would open frozen at
    // whatever the buffer held when the panes were built.
    const openFs = touch ? createFullscreen(() => (store ?? s).source(), NO_EVENTS) : undefined;
    let menuOpenedAt = 0;

    charts = Array.from({ length: channelCount(source) }, (_, i) => {
      const el = document.getElementById(`sig-${String(i)}`);
      if (el === null) return undefined;
      const c = echarts.init(el, null, { renderer: 'canvas' });
      c.setOption(buildBubbleOption(source, NO_EVENTS, i, touch));
      c.group = CONNECT_GROUP;
      if (openFs !== undefined) {
        c.getZr().on('click', () => {
          if (Date.now() - menuOpenedAt < TAP_SUPPRESS_MS) return;
          openFs(i);
        });
      }
      // Any zoom the user drives drops out of follow mode, so scrolling back
      // through the night isn't yanked forward again on the next frame. Our own
      // follow dispatch echoes back as the same event, hence the time guard.
      c.on('dataZoom', () => {
        if (Date.now() - selfZoomAt > SELF_ZOOM_GUARD_MS && following) setFollowing(false);
      });
      return c;
    }).filter((c): c is echarts.ECharts => c !== undefined);

    echarts.connect(CONNECT_GROUP);
    // No metrics service in live mode: it scores a stored recording, and this
    // one isn't finished. The brush still works, for reading a span's duration.
    const selection = wireBrushSync(charts, () => undefined);
    if (!touch) {
      armBrush(charts);
      wireCtrlZoom(charts);
    }
    wireContextMenu(charts, selection, touch, () => { menuOpenedAt = Date.now(); });
    wireMontageToggles();

    document.getElementById('rt-follow')?.addEventListener('click', () => { setFollowing(!following); });
    setFollowing(true);
    renderedPoints = -1;
    paintStatus();
  };

  const tick = (): void => {
    const s = store;
    if (s !== undefined && charts.length > 0 && s.totalPoints !== renderedPoints) {
      renderedPoints = s.totalPoints;
      const source = s.source();
      const tMax = s.latestTime();
      charts.forEach((c, i) => { c.setOption(liveUpdateOption(source, i, tMax)); });
      if (following && tMax > 0) {
        selfZoomAt = Date.now();
        // One dispatch — echarts.connect mirrors dataZoom across the group.
        charts[0]?.dispatchAction(followLiveAction(tMax));
      }
      paintStatus();
    }
    globalThis.setTimeout(tick, liveTickMs(s?.totalPoints ?? 0));
  };

  const client = connectRt(url, {
    onHello: (h) => {
      const sameSession = hello !== undefined && isSameStream(hello, h);
      hello = h;
      // A reconnect into the same recording keeps the buffers: every block
      // carries an absolute sample index, so the history already held stays
      // correctly placed and only the missed span shows as a gap.
      if (!sameSession || store === undefined) {
        store = new RtStore(h.channels);
        buildPanes(h, store);
      } else {
        paintStatus();
      }
    },
    onSamples: (frame) => { store?.append(frame); },
    onStatus: (s) => { status = s; paintStatus(); },
    onState: (state, detail) => { link = state; linkDetail = detail; paintStatus(); },
  });
  badFrames = client.badFrames;

  window.addEventListener('resize', () => { charts.forEach((c) => { c.resize(); }); });
  tick();
}

/** No `?meta=` param: show the landing page listing every study `deploy.py` has published. */
async function runLanding(app: HTMLElement): Promise<void> {
  const studies = await loadStudies(new URL('studies.json', window.location.href).href);
  app.innerHTML = renderLanding(studies);
}

function main(): void {
  const app = document.getElementById('app');
  if (app === null) return;
  const param = new URLSearchParams(window.location.search).get('meta');
  const rtUrl = rtWsUrl(window.location);

  // RT wins over `?meta=`: asking for a live stream is explicit, and the two
  // are different recordings by definition — one in progress, one finished.
  if (rtUrl !== undefined) {
    runRealtime(app, rtUrl);
  } else if (param === null) {
    runLanding(app).catch((err: unknown) => {
      app.innerHTML = `<pre class="fatal">Error loading studies:\n${String(err)}</pre>`;
      console.error(err);
    });
  } else {
    run(app, new URL(param, window.location.href).href).catch((err: unknown) => {
      app.innerHTML = `<pre class="fatal">Error loading recording:\n${String(err)}</pre>`;
      console.error(err);
    });
  }

  if ('serviceWorker' in navigator) {
    window.addEventListener('load', () => { void navigator.serviceWorker.register('sw.js'); });
  }
}

main();
