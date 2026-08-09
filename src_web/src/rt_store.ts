/**
 * Accumulates streamed samples into one growable interleaved `[t, v, t, v, …]`
 * buffer per channel, and exposes them as a `SignalSource` the existing chart
 * builders read unchanged.
 *
 * Interleaved-and-growable rather than a pair of arrays rebuilt per render,
 * because a live session keeps everything since connect: 11 channels over a
 * night is ~17 M samples, and `chart.ts`'s `zip()` would allocate a JS array
 * object per point. Here each render tick hands ECharts a `subarray` view of
 * the buffer the samples already live in — zero copies, zero garbage.
 *
 * State and mutation are the whole point of this module; it is the imperative
 * shell for the live path, with the decisions (parsing, the affine map, gap
 * arithmetic) factored out into `rt_protocol.ts` where they are pure.
 */

import { affineMap, gapBefore } from './rt_protocol';
import type { RtChannelDef, RtSamples } from './rt_types';
import type { Series, SignalSource } from './signals';

/** Points held before the first growth. 4096 ≈ 82 s of a 50 Hz channel. */
const INITIAL_POINTS = 4096;

/** One channel's buffer: times and values interleaved, plus its decode state. */
class ChannelBuffer {
  private buf: Float64Array = new Float64Array(INITIAL_POINTS * 2);
  /** Points written so far (each is two `buf` slots). */
  private n = 0;
  /** Absolute sample index expected next — the anchor gap detection works from. */
  private nextIndex = 0;
  /** Breaks inserted for missing samples, for the status strip's diagnostics. */
  private breaks = 0;

  private readonly toPhysical: (digital: number) => number;
  private readonly period: number;

  constructor(private readonly def: RtChannelDef, private readonly maxPoints: number) {
    this.toPhysical = affineMap(def);
    this.period = 1 / def.sample_rate_hz;
  }

  private ensure(extraPoints: number): void {
    const needed = (this.n + extraPoints) * 2;
    if (needed <= this.buf.length) return;
    // Doubling keeps append amortised O(1); the copy cost is paid log2(N) times
    // over a whole night rather than on every frame.
    const grown = new Float64Array(Math.max(needed, this.buf.length * 2));
    grown.set(this.buf.subarray(0, this.n * 2));
    this.buf = grown;
  }

  private push(t: number, v: number): void {
    this.buf[this.n * 2] = t;
    this.buf[this.n * 2 + 1] = v;
    this.n += 1;
  }

  /** Drop the oldest points once past the cap (only runs when `maxPoints` is finite). */
  private trim(): void {
    if (this.n <= this.maxPoints) return;
    const drop = this.n - this.maxPoints;
    this.buf.copyWithin(0, drop * 2, this.n * 2);
    this.n = this.maxPoints;
  }

  /**
   * Append one block. `n0` is absolute, so this re-anchors itself: a block that
   * overlaps what we already hold has its duplicate prefix skipped, and a block
   * that starts beyond the next expected index gets a single NaN point at the
   * first missing sample's time, which ECharts renders as a break in the line
   * rather than a straight segment across data that was never received.
   */
  append(n0: number, values: readonly number[]): void {
    const missing = gapBefore(this.nextIndex, n0);
    const skip = Math.max(0, this.nextIndex - n0);
    if (skip >= values.length) return; // wholly a retransmit of samples we have

    this.ensure(values.length - skip + (missing > 0 ? 1 : 0));

    if (missing > 0 && this.n > 0) {
      this.push(this.nextIndex * this.period, Number.NaN);
      this.breaks += 1;
    }

    // Loops rather than array methods: this runs on every frame for every
    // channel, and the whole point of the interleaved buffer is to avoid
    // per-sample allocation on the hot path. Indexing needs no guard —
    // `parseBlock` has already established every element is a finite number.
    for (let i = skip; i < values.length; i++) {
      this.push((n0 + i) * this.period, this.toPhysical(values[i]));
    }

    this.nextIndex = n0 + values.length;
    this.trim();
  }

  /**
   * Zero-copy view of the points written so far. Valid until the next `append`.
   * `declared` because the device named this channel in `hello`: its pane
   * belongs on screen from the start, not once the first sample happens to land.
   */
  series(): Series {
    return { points: this.buf.subarray(0, this.n * 2), declared: true };
  }

  get pointCount(): number { return this.n; }
  get breakCount(): number { return this.breaks; }
  get channel(): RtChannelDef { return this.def; }

  /** Elapsed seconds of the most recent sample, or 0 before anything arrives. */
  latestTime(): number {
    return this.n === 0 ? 0 : (this.nextIndex - 1) * this.period;
  }
}

export interface RtStoreOptions {
  /**
   * Cap on points retained per channel. Unlimited by default — a live session
   * keeps everything since connect, which is what makes the whole night
   * scrollable. Set a finite value to trade history for a memory ceiling.
   */
  readonly maxPoints?: number;
}

export class RtStore {
  private readonly buffers: ReadonlyMap<string, ChannelBuffer>;

  constructor(channels: readonly RtChannelDef[], opts: RtStoreOptions = {}) {
    const cap = opts.maxPoints ?? Number.POSITIVE_INFINITY;
    this.buffers = new Map(channels.map((c) => [c.name, new ChannelBuffer(c, cap)]));
  }

  /** Fold one sample frame in. Blocks naming a channel `hello` never declared are ignored. */
  append(frame: RtSamples): void {
    for (const [name, block] of Object.entries(frame.blocks)) {
      this.buffers.get(name)?.append(block.n0, block.v);
    }
  }

  /** The data surface `chart.ts` reads — rebuilt per render tick, but only of tiny views. */
  source(): SignalSource {
    return Object.fromEntries(
      Array.from(this.buffers, ([name, buf]) => [name, buf.series()]),
    );
  }

  get channels(): readonly RtChannelDef[] {
    return Array.from(this.buffers.values(), (b) => b.channel);
  }

  /** Elapsed seconds of the newest sample across all channels — drives follow-live. */
  latestTime(): number {
    return Array.from(this.buffers.values()).reduce((max, b) => Math.max(max, b.latestTime()), 0);
  }

  get totalPoints(): number {
    return Array.from(this.buffers.values()).reduce((sum, b) => sum + b.pointCount, 0);
  }

  /** Total line breaks inserted for missing samples — a visible measure of stream loss. */
  get breakCount(): number {
    return Array.from(this.buffers.values()).reduce((sum, b) => sum + b.breakCount, 0);
  }
}
