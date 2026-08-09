/**
 * The `protosom.rt` wire protocol, as pure functions: parse a frame, and turn a
 * channel declaration into its EDF+ digital→physical map. No socket, no DOM, no
 * state — `rt_client.ts` owns the I/O and `rt_store.ts` owns the buffers, so
 * everything decision-shaped here is unit-testable on plain strings.
 *
 * Parsing is deliberately total: every malformed frame comes back as an
 * `RtParseError` rather than throwing. A device that ships a truncated or
 * garbled frame must cost the viewer one dropped sample block, not the session.
 */

import { RT_PROTOCOL } from './rt_types';
import type {
  RtBlock, RtChannelDef, RtHello, RtParsed, RtSamples, RtStatus,
} from './rt_types';

// ── narrow `unknown` (JSON.parse output) without ever trusting it ────────────

const isRecord = (v: unknown): v is Readonly<Record<string, unknown>> =>
  typeof v === 'object' && v !== null && !Array.isArray(v);

const isNum = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v);
const isStr = (v: unknown): v is string => typeof v === 'string';

const isNumArray = (v: unknown): v is readonly number[] =>
  Array.isArray(v) && (v as readonly unknown[]).every(isNum);

/** Optional string field: absent/wrong-typed degrades to `fallback` rather than rejecting the frame. */
const str = (v: unknown, fallback = ''): string => (isStr(v) ? v : fallback);
const bool = (v: unknown, fallback = false): boolean => (typeof v === 'boolean' ? v : fallback);
const num = (v: unknown, fallback = 0): number => (isNum(v) ? v : fallback);

const err = (reason: string): RtParsed => ({ type: 'error', reason });

// ── protocol version ─────────────────────────────────────────────────────────

const majorOf = (protocol: string): string => {
  const [major = ''] = protocol.substring(protocol.lastIndexOf('/') + 1).split('.');
  return major;
};

/**
 * Same family and same major version. Minor/patch bumps are additive by
 * contract (new optional fields, new channels), so an older client keeps
 * working against a newer device.
 */
export function isCompatibleProtocol(protocol: string): boolean {
  const family = (p: string): string => p.substring(0, p.lastIndexOf('/'));
  return family(protocol) === family(RT_PROTOCOL) && majorOf(protocol) === majorOf(RT_PROTOCOL);
}

// ── channel declarations ─────────────────────────────────────────────────────

function parseChannel(v: unknown): RtChannelDef | undefined {
  if (!isRecord(v)) return undefined;
  const { name, sample_rate_hz, digital_min, digital_max, physical_min, physical_max } = v;

  if (!isStr(name) || name === '') return undefined;
  if (!isNum(sample_rate_hz) || sample_rate_hz <= 0) return undefined;
  if (!isNum(digital_min) || !isNum(digital_max)) return undefined;
  if (!isNum(physical_min) || !isNum(physical_max)) return undefined;
  // A zero-width digital range has no affine map back to physical units; a
  // producer that declares one has a broken channel table, not a scaling quirk.
  if (digital_max === digital_min) return undefined;

  return {
    name,
    edf_label: str(v.edf_label, name),
    transducer: str(v.transducer),
    unit: str(v.unit),
    sample_rate_hz,
    digital_min,
    digital_max,
    physical_min,
    physical_max,
  };
}

/**
 * The EDF+ header's affine map, `physical = gain·digital + offset` with
 * `gain = (pmax − pmin)/(dmax − dmin)` and `offset = pmin − gain·dmin` — the
 * same arithmetic `src_python/edf_reader.py` applies host-side. Channels whose
 * physical and digital ranges coincide (ECG, RR) collapse to the identity, so
 * their streamed values are the recorded integers untouched.
 */
export function affineMap(ch: RtChannelDef): (digital: number) => number {
  const gain = (ch.physical_max - ch.physical_min) / (ch.digital_max - ch.digital_min);
  const offset = ch.physical_min - gain * ch.digital_min;
  return (digital: number): number => gain * digital + offset;
}

// ── frames ───────────────────────────────────────────────────────────────────

function parseHello(v: Readonly<Record<string, unknown>>): RtParsed {
  const protocol = str(v.protocol);
  if (!isCompatibleProtocol(protocol)) {
    return err(`incompatible stream protocol "${protocol}" (this viewer speaks ${RT_PROTOCOL})`);
  }
  if (!Array.isArray(v.channels)) return err('hello: channels is not an array');

  const parsed = (v.channels as readonly unknown[]).map(parseChannel);
  const bad = parsed.findIndex((c) => c === undefined);
  if (bad !== -1) return err(`hello: channel ${String(bad)} is malformed`);
  const channels = parsed.filter((c): c is RtChannelDef => c !== undefined);
  if (channels.length === 0) return err('hello: no channels declared');

  const hello: RtHello = {
    type: 'hello',
    protocol,
    device_uid: str(v.device_uid, 'unknown'),
    recording_start_iso: str(v.recording_start_iso),
    timezone: str(v.timezone),
    recording_active: bool(v.recording_active),
    record_duration_s: num(v.record_duration_s),
    channels,
  };
  return hello;
}

function parseBlock(v: unknown): RtBlock | undefined {
  if (!isRecord(v)) return undefined;
  const { n0 } = v;
  if (!isNum(n0) || n0 < 0 || !Number.isInteger(n0)) return undefined;
  if (!isNumArray(v.v)) return undefined;
  return { n0, v: v.v };
}

function parseSamples(v: Readonly<Record<string, unknown>>): RtParsed {
  const { seq, blocks } = v;
  if (!isNum(seq)) return err('samples: missing seq');
  if (!isRecord(blocks)) return err('samples: blocks is not an object');

  const entries = Object.entries(blocks)
    .map(([name, raw]: readonly [string, unknown]) => [name, parseBlock(raw)] as const);
  const bad = entries.find(([, block]) => block === undefined);
  if (bad !== undefined) return err(`samples: block "${bad[0]}" is malformed`);

  const frame: RtSamples = {
    type: 'samples',
    seq,
    blocks: Object.fromEntries(
      entries.filter((e): e is readonly [string, RtBlock] => e[1] !== undefined),
    ),
  };
  return frame;
}

function parseStatus(v: Readonly<Record<string, unknown>>): RtParsed {
  const status: RtStatus = {
    type: 'status',
    recording: bool(v.recording),
    elapsed_s: num(v.elapsed_s),
    batt_pct: isNum(v.batt_pct) ? v.batt_pct : null,
    sd_error: bool(v.sd_error),
    dropped_frames: num(v.dropped_frames),
    clients: num(v.clients),
  };
  return status;
}

/** Parse one WebSocket text frame. Never throws — an unusable frame is an `RtParseError`. */
export function parseRtMessage(raw: string): RtParsed {
  const doc: unknown = ((): unknown => {
    try {
      return JSON.parse(raw);
    } catch {
      return undefined;
    }
  })();

  if (!isRecord(doc)) return err('frame is not a JSON object');
  const { type } = doc;
  if (type === 'hello') return parseHello(doc);
  if (type === 'samples') return parseSamples(doc);
  if (type === 'status') return parseStatus(doc);
  // Forward-compatibility: an unknown frame type is a message from a newer
  // device, not corruption. Report it as such so the caller can count it
  // separately from a genuinely broken frame if it ever wants to.
  return err(`unknown frame type "${isStr(type) ? type : typeof type}"`);
}

/**
 * How many samples are missing between the last one appended and the next
 * block's first. Negative/zero gaps (a retransmit, or a block that overlaps
 * what we already hold) report 0 — the store re-anchors on `n0` regardless, so
 * an overlap is harmless and must not be drawn as a break.
 */
export function gapBefore(nextIndex: number, n0: number): number {
  return Math.max(0, n0 - nextIndex);
}
