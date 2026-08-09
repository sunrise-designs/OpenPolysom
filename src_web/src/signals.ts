/**
 * The data surface `chart.ts`'s channel table reads from.
 *
 * Both viewer modes produce a `SignalSource`, so one channel table (and one set
 * of ECharts option builders) serves both:
 *
 *  - **batch** — `zarr_loader.ts`'s `toSignalSource()` adapts the decoded
 *    `ZarrData` (whole recording, separate `x`/`y` arrays).
 *  - **RT** — `rt_store.ts` hands over a pre-interleaved `[t, v, t, v, …]`
 *    buffer per channel. Interleaved because a live session grows without
 *    bound: rebuilding an array of `[t, v]` pairs (see `chart.ts`'s `zip`)
 *    allocates one JS array object per point, which is fine for a loaded
 *    recording and ruinous for millions of streamed samples. A subarray view of
 *    the store's own buffer costs nothing per render.
 *
 * Keys are the canonical snake_case channel names from the
 * wiki/planning/zarr-schema-spec.md §3.2 registry (`thoracic`, `ecg`,
 * `accel0_x`, …), so the two modes can never disagree about what a channel is
 * called even though they source it from different places.
 */

/** One plotted series: separate x/y arrays (batch), or an interleaved `[t, v, …]` buffer (RT). */
export interface Series {
  /** Sample times, elapsed seconds from recording start. Paired with `y`. */
  readonly x?: Float64Array;
  /** Sample values, in physical units. Paired with `x`. */
  readonly y?: ArrayLike<number>;
  /** Interleaved `[t0, v0, t1, v1, …]`. Takes precedence over `x`/`y` when present. */
  readonly points?: Float64Array;
  /**
   * Render this channel's pane even before any samples arrive.
   *
   * Batch infers a channel's existence from having data — an empty array is how
   * a wrist-only recording says "no respiratory belts". A live stream can't use
   * that rule: the device declares its channel table up front in `hello`, and a
   * 2.5 Hz channel may legitimately have sent nothing yet when the panes are
   * built. Declaring is how RT says "this channel exists, samples are coming".
   */
  readonly declared?: boolean;
}

/** Canonical channel name → series. The whole data surface a chart mode exposes. */
export type SignalSource = Readonly<Record<string, Series>>;

/** Number of plottable points in a series — 0 for absent or unpaired data. */
export function seriesLength(s: Series | undefined): number {
  if (s === undefined) return 0;
  if (s.points !== undefined) return Math.floor(s.points.length / 2);
  if (s.x === undefined || s.y === undefined) return 0;
  return Math.min(s.x.length, s.y.length);
}

/** Elapsed seconds of the last sample across every channel — the recording's x-extent. */
export function sourceTMax(src: SignalSource): number {
  return Object.values(src).reduce((max, s) => {
    const n = seriesLength(s);
    if (n === 0) return max;
    const last = s.points !== undefined ? s.points[(n - 1) * 2] : s.x?.[n - 1];
    return last === undefined || !Number.isFinite(last) ? max : Math.max(max, last);
  }, 0);
}
