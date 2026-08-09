/**
 * Types for the ProtoSom real-time stream (`protosom.rt`) — the WebSocket the
 * ESP32-C6's `rt_stream` component serves while a recording is running.
 *
 * This is the *second* read boundary the viewer has, alongside the Zarr working
 * store (see wiki/knowledge/viewer.md § RT vs batch). It carries **digital
 * integers exactly as `edfwrite_digital_samples` receives them** in
 * `ESP32-C6-heart-idf/components/logger/logger.cpp` — one streamed data point
 * per EDF+ sample. Nothing here is persisted and nothing derived is computed:
 * the EDF+ on the SD card remains the raw anchor, and the viewer still never
 * writes Zarr.
 */

/** Wire protocol version this client speaks. Same-major streams are accepted. */
export const RT_PROTOCOL = 'protosom.rt/1.0.0';

/** Which of the viewer's two data paths is in play. */
export type ViewMode = 'batch' | 'rt';

/** WebSocket link state, surfaced in the RT status strip. */
export type RtLinkState = 'connecting' | 'live' | 'reconnecting' | 'failed';

/**
 * One channel's declaration, mirroring a row of `logger.cpp`'s `SigDef` table.
 * `digital_*`/`physical_*` are the EDF+ header's affine map — the client applies
 * it (see `rt_protocol.ts`'s `affineMap`) rather than trusting the producer to
 * have calibrated, so the wire stays bit-identical to the recorded samples.
 */
export interface RtChannelDef {
  /** Canonical snake_case name (zarr-schema-spec §3.2), e.g. `thoracic`, `accel0_x`. */
  readonly name: string;
  /** Verbatim EDF+ header label, e.g. `Thoracic`. */
  readonly edf_label: string;
  readonly transducer: string;
  /** EDF+ physical dimension, e.g. `mg`, `mbar`, `ADC`, `counts`, `ms`. */
  readonly unit: string;
  readonly sample_rate_hz: number;
  readonly digital_min: number;
  readonly digital_max: number;
  readonly physical_min: number;
  readonly physical_max: number;
}

/** Sent once on connect: everything needed to build the panes and decode the samples. */
export interface RtHello {
  readonly type: 'hello';
  readonly protocol: string;
  readonly device_uid: string;
  /** Recording start (the EDF+ header's whole-second start datetime), ISO-8601. */
  readonly recording_start_iso: string;
  readonly timezone: string;
  /** False when the device is streaming but not writing an EDF+ (e.g. no SD card). */
  readonly recording_active: boolean;
  readonly record_duration_s: number;
  readonly channels: readonly RtChannelDef[];
}

/**
 * One channel's samples in a frame. `n0` is the **absolute** index of `v[0]`
 * since recording start, so sample `i` sits at `(n0 + i) / sample_rate_hz`
 * seconds — the same elapsed position it occupies in the EDF+. Absolute rather
 * than relative so a dropped frame or a reconnect re-anchors itself instead of
 * silently sliding every later sample forward.
 */
export interface RtBlock {
  readonly n0: number;
  readonly v: readonly number[];
}

/** Repeated sample frame, ~10/second. Channels sampled slower than the frame period may be absent. */
export interface RtSamples {
  readonly type: 'samples';
  /** Monotonic frame counter. A skip means the producer dropped frames under backpressure. */
  readonly seq: number;
  readonly blocks: Readonly<Record<string, RtBlock>>;
}

/** Device health, ~1/second. Feeds the RT status strip. */
export interface RtStatus {
  readonly type: 'status';
  readonly recording: boolean;
  readonly elapsed_s: number;
  /** Null when the battery gauge is unavailable. */
  readonly batt_pct: number | null;
  /** True once the logger stopped itself after an SD write failure. */
  readonly sd_error: boolean;
  /** Frames the device discarded because the send queue was full. */
  readonly dropped_frames: number;
  readonly clients: number;
}

export type RtMessage = RtHello | RtSamples | RtStatus;

/** A frame that could not be understood. Counted and dropped — never fatal. */
export interface RtParseError {
  readonly type: 'error';
  readonly reason: string;
}

export type RtParsed = RtMessage | RtParseError;
