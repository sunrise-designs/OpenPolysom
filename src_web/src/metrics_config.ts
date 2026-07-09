/**
 * Resolves the base URL of the windowed clinical-metrics service (see
 * `metrics_client.ts`). Decoupled from the viewer's own origin: Netlify (the
 * static deploy target, `src_python/deploy.py`) can't host the long-running
 * FastAPI process (`src_python/metrics_service.py`), so in the cloud case
 * this points at a different host entirely.
 *
 * Precedence: `?metrics=` query override, then `window.PROTOSOM_METRICS_URL`
 * (set by the un-bundled static `metrics-config.js`, loaded before the chart
 * bundle — same pattern as `styles.css`/`sw.js`/`manifest.webmanifest`).
 * Returns `undefined` when neither is set, meaning the feature is quietly
 * unavailable (no metrics card rendered).
 */
export function metricsBaseUrl(location: Readonly<Pick<Location, 'href'>>): string | undefined {
  const fromQuery = new URL(location.href).searchParams.get('metrics');
  if (fromQuery !== null && fromQuery !== '') return fromQuery;

  const fromWindow = (globalThis as { readonly PROTOSOM_METRICS_URL?: string }).PROTOSOM_METRICS_URL;
  return fromWindow === '' ? undefined : fromWindow;
}
