import { describe, it, expect } from 'vitest';
import { channelCount, channelMeta, buildBubbleOption } from '../src/chart';
import type { ZarrData, EventsDoc } from '../src/types';

const n = (len: number, fill = 1): Float64Array => new Float64Array(len).fill(fill);
const f32 = (len: number, fill = 1): Float32Array => new Float32Array(len).fill(fill);

/** A legacy single-accelerometer store (e.g. tools/make_fixture.py's sample) — no Accel1/combined arrays. */
const legacyZarr: ZarrData = {
  t: n(10, 1),
  accel_x: f32(10),
  accel_y: f32(10),
  accel_z: f32(10),
  accel_mag: f32(10),
  accel1_mag: new Float32Array(0),
  accel_combined_mag: new Float32Array(0),
  rr: f32(10, 800),
  rr_t: n(10, 1),
  hrv_t: new Float64Array(0),
  hrv_rmssd: new Float32Array(0),
};

/** A dual-accelerometer store — both legs scored, per read_log.py's --count path. */
const dualZarr: ZarrData = {
  ...legacyZarr,
  accel1_mag: f32(10, 2),
  accel_combined_mag: f32(10, 2),
};

describe('chart channel filtering (Accel1 / combined bilateral)', () => {
  it('hides the Accel1/combined panes when the store has no second accelerometer', () => {
    expect(channelCount(legacyZarr)).toBe(6);
    const names = channelMeta(legacyZarr).map((m) => m.name);
    expect(names).not.toContain('Accel1 mag (leg 2)');
    expect(names).not.toContain('Combined LM (bilateral)');
  });

  it('shows the Accel1/combined panes when both accelerometers were scored', () => {
    expect(channelCount(dualZarr)).toBe(8);
    const names = channelMeta(dualZarr).map((m) => m.name);
    expect(names).toContain('Accel1 mag (leg 2)');
    expect(names).toContain('Combined LM (bilateral)');
  });

  const events: EventsDoc = {
    schema: 'protosom.events',
    schema_version: '1.0.0',
    recording_id: 'test',
    scorings: [
      {
        scoring_id: 'plm-auto-v0-accel0',
        events: [{ id: 'a0_ev1', type: 'limb_movement', onset_s: 1, duration_s: 1, channels: ['accel_mag'] }],
        groups: [],
      },
      {
        scoring_id: 'plm-auto-v0-accel1',
        events: [{ id: 'a1_ev1', type: 'limb_movement', onset_s: 2, duration_s: 1, channels: ['accel1_mag'] }],
        groups: [],
      },
      {
        scoring_id: 'plm-auto-v0-combined',
        events: [{ id: 'cb_ev1', type: 'limb_movement', onset_s: 3, duration_s: 1, channels: ['accel_combined_mag'] }],
        groups: [],
      },
    ],
  };

  const markAreaData = (index: number): readonly unknown[] => {
    const opt = buildBubbleOption(dualZarr, events, index);
    const series = (opt.series as readonly { readonly markArea?: { readonly data?: readonly unknown[] } }[])[0];
    return series?.markArea?.data ?? [];
  };

  it("only overlays each leg's own LM events on its own pane, not the other legs'", () => {
    // CHANNELS order: 0 RR, 1 Accel mag, 2 HRV, 3 Accel X, 4 Accel Y, 5 Accel Z, 6 Accel1 mag, 7 Combined LM
    expect(markAreaData(1)).toHaveLength(1); // Accel0's own LM only
    expect(markAreaData(6)).toHaveLength(1); // Accel1's own LM only
    expect(markAreaData(7)).toHaveLength(1); // combined's own LM only
  });

  it('non-overlay panes (raw axes, RR, HRV) render no markArea at all', () => {
    const opt = buildBubbleOption(dualZarr, events, 3); // Accel X
    const series = (opt.series as readonly { readonly markArea?: unknown }[])[0];
    expect(series?.markArea).toBeUndefined();
  });
});
