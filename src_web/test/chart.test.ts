import { describe, it, expect } from 'vitest';
import { channelCount, channelIndex, channelMeta, buildBubbleOption, zoomToRangeAction, followLiveAction, NO_EVENTS } from '../src/chart';
import { RtStore } from '../src/rt_store';
import { EDF_CHANNELS } from './helpers/rtFixtures';
import { toSignalSource } from '../src/zarr_loader';
import type { SignalSource } from '../src/signals';
import type { ZarrData, EventsDoc } from '../src/types';

const n = (len: number, fill = 1): Float64Array => new Float64Array(len).fill(fill);
const f32 = (len: number, fill = 1): Float32Array => new Float32Array(len).fill(fill);

/** A legacy single-accelerometer store (e.g. tools/make_fixture.py's sample) — no Accel1/combined/respiratory arrays. */
const legacyStore: ZarrData = {
  t: n(10, 1),
  accel_x: f32(10),
  accel_y: f32(10),
  accel_z: f32(10),
  accel_mag: f32(10),
  accel1_mag: new Float32Array(0),
  accel_combined_mag: new Float32Array(0),
  thoracic: new Float32Array(0),
  abdomen: new Float32Array(0),
  flow: new Float32Array(0),
  rr: f32(10, 800),
  rr_t: n(10, 1),
  hrv_t: new Float64Array(0),
  hrv_rmssd: new Float32Array(0),
};

/** A dual-accelerometer store — both legs scored, per read_log.py's --count path. Still no respiratory device. */
const dualStore: ZarrData = {
  ...legacyStore,
  accel1_mag: f32(10, 2),
  accel_combined_mag: f32(10, 2),
};

/** The ESP32-C6 11-channel case: dual accelerometer AND Thoracic/Abdomen/Flow both present at once. */
const fullStore: ZarrData = {
  ...dualStore,
  thoracic: f32(10, 3),
  abdomen: f32(10, 4),
  flow: f32(10, 5),
};

// The chart reads a `SignalSource`, not the raw decode result, so every fixture
// goes through the same adapter the batch path uses — which keeps the canonical
// channel-name mapping (`accel_x` -> `accel0_x`, `hrv_t`/`hrv_rmssd` -> one
// series) under test rather than duplicated here.
const legacyZarr: SignalSource = toSignalSource(legacyStore);
const dualZarr: SignalSource = toSignalSource(dualStore);
const fullZarr: SignalSource = toSignalSource(fullStore);

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

describe('zoomToRangeAction (the context menu\'s "Zoom to window")', () => {
  it('zooms to exactly the selected band when it is wider than the floor', () => {
    expect(zoomToRangeAction([120, 300])).toEqual({ type: 'dataZoom', startValue: 120, endValue: 300 });
  });

  it('widens a sub-second selection about its midpoint rather than collapsing the axis', () => {
    // A few-pixel drag is a real selection (removeOnClick only clears a true click),
    // but zooming to it would leave an empty pane with no way back but the slider.
    const action = zoomToRangeAction([100, 100.2]);
    expect(action.endValue - action.startValue).toBeCloseTo(1);
    expect((action.startValue + action.endValue) / 2).toBeCloseTo(100.1);
  });
});

describe('chart channel filtering (Respiratory: Thoracic/Abdomen/Flow)', () => {
  it('hides the Respiratory panes when the store has no respiratory device', () => {
    const names = channelMeta(dualZarr).map((m) => m.name);
    expect(names).not.toContain('Thoracic');
    expect(names).not.toContain('Abdomen');
    expect(names).not.toContain('Flow');
    expect(channelIndex(dualZarr, 'Thoracic')).toBe(-1);
  });

  it('shows the Respiratory panes when Thoracic/Abdomen/Flow are present', () => {
    expect(channelCount(fullZarr)).toBe(11); // 8 (dual accel) + Thoracic/Abdomen/Flow
    const names = channelMeta(fullZarr).map((m) => m.name);
    expect(names).toContain('Thoracic');
    expect(names).toContain('Abdomen');
    expect(names).toContain('Flow');
  });

  it(
    'resolves every optional group to its real rendered index by name, not a fixed trailing ' +
      'position — the bug: Accel1/Combined and Thoracic/Abdomen/Flow can both be present at ' +
      'once (e.g. the ESP32-C6 11-channel device), so neither group is reliably "the last N" ' +
      'channels, which is exactly why the Respiratory montage switch never turned on before',
    () => {
      const idx = (name: string): number => channelIndex(fullZarr, name);
      const names = channelMeta(fullZarr).map((m) => m.name);

      // Every name resolves to a real, distinct position.
      for (const name of ['Thoracic', 'Abdomen', 'Flow', 'Accel1 mag (leg 2)', 'Combined LM (bilateral)']) {
        expect(idx(name)).toBeGreaterThanOrEqual(0);
      }
      const indices = ['Thoracic', 'Abdomen', 'Flow', 'Accel1 mag (leg 2)', 'Combined LM (bilateral)'].map(idx);
      expect(new Set(indices).size).toBe(indices.length); // no two names collide on one index

      // And every resolved index actually names the channel it claims to.
      for (const name of ['Thoracic', 'Abdomen', 'Flow', 'Accel1 mag (leg 2)', 'Combined LM (bilateral)']) {
        expect(names[idx(name)]).toBe(name);
      }
    },
  );
});

describe('chart channel binding in RT (live) mode', () => {
  const liveSource = (): SignalSource => {
    const store = new RtStore(EDF_CHANNELS);
    store.append({
      type: 'samples',
      seq: 1,
      blocks: Object.fromEntries(EDF_CHANNELS.map((c) => [c.name, { n0: 0, v: [1, 2, 3] }])),
    });
    return store.source();
  };

  it('renders one pane per streamed EDF+ channel, in EDF signal order within the table', () => {
    const names = channelMeta(liveSource()).map((m) => m.name);
    expect(names).toEqual([
      'RR · ms', 'ECG · ADC',
      'Accel X', 'Accel Y', 'Accel Z',
      'Accel1 X (leg 2)', 'Accel1 Y (leg 2)', 'Accel1 Z (leg 2)',
      'Thoracic', 'Abdomen', 'Flow',
    ]);
    expect(channelCount(liveSource())).toBe(11);
  });

  it('shows no derived panes — vector magnitude, HRV and the bilateral score need Python processing', () => {
    const names = channelMeta(liveSource()).map((m) => m.name);
    expect(names).not.toContain('Accel mag');
    expect(names).not.toContain('HRV · ms');
    expect(names).not.toContain('Accel1 mag (leg 2)');
    expect(names).not.toContain('Combined LM (bilateral)');
  });

  it('shows the ECG and Accel1 axis panes that no derived store carries', () => {
    const src = liveSource();
    expect(channelIndex(src, 'ECG · ADC')).toBeGreaterThanOrEqual(0);
    expect(channelIndex(src, 'Accel1 X (leg 2)')).toBeGreaterThanOrEqual(0);
    // …and those panes stay hidden in batch, where no such array exists.
    expect(channelIndex(dualZarr, 'ECG · ADC')).toBe(-1);
    expect(channelIndex(fullZarr, 'Accel1 X (leg 2)')).toBe(-1);
  });

  it('draws no markArea in live mode — there is no scoring to overlay yet', () => {
    const src = liveSource();
    for (let i = 0; i < channelCount(src); i++) {
      const opt = buildBubbleOption(src, NO_EVENTS, i);
      const series = (opt.series as readonly { readonly markArea?: unknown }[])[0];
      expect(series?.markArea).toBeUndefined();
    }
  });

  it('hands ECharts the interleaved buffer directly, with the dimensions a typed array needs', () => {
    // The zero-copy path: no per-point array objects, which is what makes an
    // unbounded live session affordable. `dimensions` is what tells ECharts the
    // flat buffer is two values per point.
    const opt = buildBubbleOption(liveSource(), NO_EVENTS, 1); // ECG
    const series = (opt.series as readonly { readonly data?: unknown; readonly dimensions?: unknown }[])[0];
    expect(series?.data).toBeInstanceOf(Float64Array);
    expect(series?.dimensions).toEqual(['t', 'v']);
  });

  it('keeps the batch path on plain [t, v] pairs, unchanged', () => {
    const opt = buildBubbleOption(dualZarr, NO_EVENTS, 1);
    const series = (opt.series as readonly { readonly data?: unknown; readonly dimensions?: unknown }[])[0];
    expect(Array.isArray(series?.data)).toBe(true);
    expect(series?.dimensions).toBeUndefined();
  });

  it('declares a pane for a channel that has not sent a sample yet', () => {
    // RR is 2.5 Hz: most 100 ms frames carry nothing for it, and the panes are
    // built from `hello` before any frame at all. An empty pane is correct; a
    // missing one would appear later and reshuffle every index after it.
    const store = new RtStore(EDF_CHANNELS);
    expect(channelCount(store.source())).toBe(11);
  });

  it('follows the newest samples with a window that ends at the live edge', () => {
    expect(followLiveAction(300, 60)).toEqual({ type: 'dataZoom', startValue: 240, endValue: 300 });
    // Early in a session there is less history than the window; clamp at 0
    // rather than scrolling to negative time.
    expect(followLiveAction(12, 60)).toEqual({ type: 'dataZoom', startValue: 0, endValue: 12 });
  });
});
