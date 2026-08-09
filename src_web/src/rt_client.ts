/**
 * The WebSocket edge of RT mode: opens the device's stream, hands every frame
 * to `rt_protocol.ts` to be parsed, and reconnects when the link drops.
 *
 * All the judgement lives in `rt_protocol.ts`; this file is the imperative
 * shell — sockets, timers, and the reconnect state machine.
 *
 * Reconnect matters more here than in a typical web app: the producer is a
 * battery-powered device on its own access point, sitting on a patient who
 * rolls over. A dropped link is expected, not exceptional, and the recording
 * on the SD card continues regardless — so the client backs off and keeps
 * trying rather than declaring the session over. Because every sample block
 * carries its absolute index (`RtBlock.n0`), a reconnection re-anchors exactly
 * and the missing span renders as a break rather than a distortion.
 */

import { parseRtMessage } from './rt_protocol';
import type { RtHello, RtLinkState, RtSamples, RtStatus } from './rt_types';

const INITIAL_BACKOFF_MS = 500;
const MAX_BACKOFF_MS = 15_000;
const BACKOFF_FACTOR = 1.8;

export interface RtClientHandlers {
  readonly onHello: (hello: RtHello) => void;
  readonly onSamples: (frame: RtSamples) => void;
  readonly onStatus: (status: RtStatus) => void;
  readonly onState: (state: RtLinkState, detail?: string) => void;
}

export interface RtClient {
  /** Stop reconnecting and close the socket. Idempotent. */
  close: () => void;
  /** Frames that failed to parse since connect — surfaced in the status strip. */
  badFrames: () => number;
}

/**
 * Connect and keep connected. Returns immediately; everything arrives through
 * the handlers.
 *
 * `onHello` may fire more than once — a reconnect re-declares the channel table
 * — so the caller must treat it as "the stream (re)started" rather than
 * "first contact". A device that restarts mid-session comes back with a new
 * `recording_start_iso`, which is exactly the case the caller needs to notice.
 */
export function connectRt(url: string, handlers: RtClientHandlers): RtClient {
  let socket: WebSocket | undefined;
  let backoff = INITIAL_BACKOFF_MS;
  // `ReturnType<...>` rather than `number`: this file is type-checked with
  // @types/node in scope, where setTimeout hands back a Timeout object.
  let retryTimer: ReturnType<typeof globalThis.setTimeout> | undefined;
  let closed = false;
  let bad = 0;

  const scheduleRetry = (reason: string): void => {
    if (closed) return;
    handlers.onState('reconnecting', `${reason} — retrying in ${(backoff / 1000).toFixed(1)}s`);
    retryTimer = globalThis.setTimeout(open, backoff);
    backoff = Math.min(MAX_BACKOFF_MS, backoff * BACKOFF_FACTOR);
  };

  function open(): void {
    if (closed) return;
    handlers.onState('connecting');

    const ws = ((): WebSocket | undefined => {
      try {
        return new WebSocket(url);
      } catch {
        return undefined;
      }
    })();
    // A malformed URL throws synchronously and would otherwise kill the whole
    // page load; a bad address the user typed is a reconnect case, not a crash.
    if (ws === undefined) {
      scheduleRetry('bad stream address');
      return;
    }
    socket = ws;

    ws.addEventListener('open', () => {
      backoff = INITIAL_BACKOFF_MS; // a link that came up is a link worth retrying fast
      handlers.onState('live');
    });

    ws.addEventListener('message', (ev: MessageEvent<unknown>) => {
      // Text frames only — the protocol is JSON (wiki/knowledge/viewer.md § RT).
      if (typeof ev.data !== 'string') { bad += 1; return; }
      const msg = parseRtMessage(ev.data);
      if (msg.type === 'error') { bad += 1; return; }
      if (msg.type === 'hello') handlers.onHello(msg);
      else if (msg.type === 'samples') handlers.onSamples(msg);
      else handlers.onStatus(msg);
    });

    ws.addEventListener('close', () => { scheduleRetry('stream closed'); });
    // 'error' is always followed by 'close', so retrying here too would double
    // the timers. Just record why the close is about to happen.
    ws.addEventListener('error', () => { handlers.onState('reconnecting', 'stream error'); });
  }

  open();

  return {
    close: (): void => {
      closed = true;
      if (retryTimer !== undefined) globalThis.clearTimeout(retryTimer);
      socket?.close();
      socket = undefined;
    },
    badFrames: (): number => bad,
  };
}
