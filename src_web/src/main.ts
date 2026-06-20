import * as echarts from 'echarts';
import { loadMeta, loadEvents, loadZarr, httpStore } from './zarr_loader';
import { buildBubbleOption, buildSingleChannelOption, channelCount } from './chart';
import { renderShell } from './shell';
import { buildNarrative } from './narrative';
import type { Meta, EventsDoc, ZarrData } from './types';

const DEFAULT_META = 'public/sample/meta.json';
const CONNECT_GROUP = 'protosom-signals';

function baseDirOf(url: string): string {
  return url.substring(0, url.lastIndexOf('/') + 1);
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

  // eslint-disable-next-line functional/no-let -- imperative shell: full-screen chart lifecycle
  let fsChart: echarts.ECharts | undefined;

  const close = (): void => {
    const orientation = screen.orientation as ScreenOrientation & { unlock?: () => void };
    try { orientation.unlock?.(); } catch { /* unsupported */ }
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
    fsChart = echarts.init(canvas, null, { renderer: 'canvas' });
    fsChart.setOption(buildSingleChannelOption(zarr, events, index));
    void (async (): Promise<void> => {
      try { await overlay.requestFullscreen(); } catch { /* unsupported */ }
      try { await screen.orientation.lock('landscape'); } catch { /* unsupported (e.g. iOS) */ }
      fsChart?.resize();
    })();
  };
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
