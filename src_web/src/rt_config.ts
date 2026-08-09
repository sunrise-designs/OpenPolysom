/**
 * Resolves the WebSocket URL of the device's real-time stream.
 *
 * Same precedence and same reasoning as `metrics_config.ts`: a `?rt=` query
 * override first, then `window.PROTOSOM_RT_URL` (set by the un-bundled static
 * `rt-config.js`, loaded before the chart bundle). Decoupled from the viewer's
 * own origin because the two are almost never co-hosted — the viewer is a
 * static PWA served from Netlify (or from cache), while the stream comes off
 * the ESP32-C6's own access point at a link-local address.
 *
 * Returns `undefined` when neither is set, which is the normal batch case: the
 * viewer opens a stored recording via `?meta=` and never touches RT mode.
 *
 * `?rt=` accepts a bare host too (`?rt=192.168.4.1`), since the AP's address is
 * the one thing a user might reasonably type by hand — it is expanded to the
 * full `ws://<host>/rt`.
 */

/** Path the device's `rt_stream` component serves the WebSocket on. */
export const RT_PATH = '/rt';

function normalize(value: string): string {
  if (value.startsWith('ws://') || value.startsWith('wss://')) return value;
  // An http(s) URL is a natural thing to paste; swap the scheme rather than reject it.
  if (value.startsWith('http://')) return `ws://${value.substring('http://'.length)}`;
  if (value.startsWith('https://')) return `wss://${value.substring('https://'.length)}`;
  return `ws://${value}${value.includes('/') ? '' : RT_PATH}`;
}

export function rtWsUrl(location: Readonly<Pick<Location, 'href'>>): string | undefined {
  const fromQuery = new URL(location.href).searchParams.get('rt');
  if (fromQuery !== null && fromQuery !== '') return normalize(fromQuery);

  const fromWindow = (globalThis as { readonly PROTOSOM_RT_URL?: string }).PROTOSOM_RT_URL;
  return fromWindow === undefined || fromWindow === '' ? undefined : normalize(fromWindow);
}
