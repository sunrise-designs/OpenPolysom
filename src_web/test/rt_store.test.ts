import { describe, it, expect } from 'vitest';
import { RtStore } from '../src/rt_store';
import { seriesLength } from '../src/signals';
import type { RtSamples } from '../src/rt_types';
import { EDF_CHANNELS, channel } from './helpers/rtFixtures';

const samples = (blocks: RtSamples['blocks'], seq = 1): RtSamples => ({ type: 'samples', seq, blocks });

/** Read a channel's interleaved buffer back as [t, v] pairs. */
const pairs = (store: RtStore, name: string): readonly (readonly [number, number])[] => {
  const pts = store.source()[name]?.points ?? new Float64Array(0);
  return Array.from({ length: pts.length / 2 }, (_, i) => [pts[i * 2] ?? 0, pts[i * 2 + 1] ?? 0] as const);
};

describe('RtStore — timing', () => {
  it('places each sample at (n0 + i) / rate — the same elapsed second it occupies in the EDF+', () => {
    const store = new RtStore(EDF_CHANNELS);
    store.append(samples({ ecg: { n0: 1000, v: [0, 0, 0] } }));
    // ECG is 100 Hz, so sample 1000 is at 10 s, and its successors 10 ms apart.
    expect(pairs(store, 'ecg').map((p) => p[0])).toEqual([10, 10.01, 10.02]);
  });

  it('gives a 2.5 Hz channel 400 ms steps, not the frame period', () => {
    const store = new RtStore(EDF_CHANNELS);
    store.append(samples({ rr: { n0: 10, v: [800, 810] } }));
    expect(pairs(store, 'rr').map((p) => p[0])).toEqual([4, 4.4]);
  });

  it('starts wherever the stream does — connecting mid-recording is not sample zero', () => {
    const store = new RtStore(EDF_CHANNELS);
    store.append(samples({ thoracic: { n0: 6420, v: [0] } }));
    expect(pairs(store, 'thoracic')[0]?.[0]).toBe(6420 / 50);
    // …and no leading break is drawn for the samples that predate the connection.
    expect(store.breakCount).toBe(0);
  });

  it('reports the newest sample time across all channels', () => {
    const store = new RtStore(EDF_CHANNELS);
    store.append(samples({ thoracic: { n0: 0, v: [1, 2, 3] }, ecg: { n0: 0, v: [1, 2, 3, 4, 5] } }));
    expect(store.latestTime()).toBeCloseTo(2 / 50); // thoracic sample 2 at 50 Hz = 0.04 s
  });
});

describe('RtStore — values', () => {
  it('applies the EDF+ affine map, so the buffer holds physical units', () => {
    const store = new RtStore(EDF_CHANNELS);
    store.append(samples({ thoracic: { n0: 0, v: [32767, -32767, 0] } }));
    const vs = pairs(store, 'thoracic').map((p) => p[1]);
    expect(vs[0]).toBeCloseTo(1e6, 6);
    expect(vs[1]).toBeCloseTo(-1e6, 6);
    expect(vs[2]).toBeCloseTo(0, 9);
  });

  it('leaves ECG untouched — its map is the identity', () => {
    const store = new RtStore(EDF_CHANNELS);
    store.append(samples({ ecg: { n0: 0, v: [2043, 4095] } }));
    expect(pairs(store, 'ecg').map((p) => p[1])).toEqual([2043, 4095]);
  });
});

describe('RtStore — gaps and re-anchoring', () => {
  it('inserts exactly one NaN break for a missed span, at the first missing sample', () => {
    const store = new RtStore(EDF_CHANNELS);
    store.append(samples({ thoracic: { n0: 0, v: [10, 20] } }));
    store.append(samples({ thoracic: { n0: 52, v: [30] } }, 2)); // samples 2..51 never arrived

    const got = pairs(store, 'thoracic');
    expect(store.breakCount).toBe(1);
    expect(got).toHaveLength(4); // 2 + break + 1
    expect(got[2]?.[0]).toBeCloseTo(2 / 50); // the break sits where the data stops
    expect(Number.isNaN(got[2]?.[1] ?? 0)).toBe(true);
    expect(got[3]?.[0]).toBeCloseTo(52 / 50); // and the next real sample is at its true time
  });

  it('does not break the line when frames arrive contiguously', () => {
    const store = new RtStore(EDF_CHANNELS);
    // ECG, because its affine map is the identity — this asserts on continuity,
    // not on scaling, which `applies the EDF+ affine map` covers separately.
    store.append(samples({ ecg: { n0: 0, v: [1, 2] } }));
    store.append(samples({ ecg: { n0: 2, v: [3, 4] } }, 2));
    expect(store.breakCount).toBe(0);
    expect(pairs(store, 'ecg').map((p) => p[1])).toEqual([1, 2, 3, 4]);
  });

  it('skips the duplicate prefix of an overlapping block instead of double-plotting it', () => {
    const store = new RtStore(EDF_CHANNELS);
    store.append(samples({ ecg: { n0: 0, v: [1, 2, 3] } }));
    store.append(samples({ ecg: { n0: 1, v: [2, 3, 4, 5] } }, 2)); // re-sends samples 1..2
    expect(pairs(store, 'ecg').map((p) => p[1])).toEqual([1, 2, 3, 4, 5]);
    expect(store.breakCount).toBe(0);
  });

  it('ignores a block that is wholly a retransmit', () => {
    const store = new RtStore(EDF_CHANNELS);
    store.append(samples({ ecg: { n0: 0, v: [1, 2, 3] } }));
    store.append(samples({ ecg: { n0: 0, v: [1, 2] } }, 2));
    expect(pairs(store, 'ecg')).toHaveLength(3);
  });
});

describe('RtStore — buffers', () => {
  it('grows past its initial capacity without losing or reordering samples', () => {
    const store = new RtStore(EDF_CHANNELS);
    const total = 10_000; // well past INITIAL_POINTS (4096)
    for (let i = 0; i < total; i += 100) {
      store.append(samples({ ecg: { n0: i, v: Array.from({ length: 100 }, (_, k) => (i + k) % 4096) } }, i));
    }
    const got = pairs(store, 'ecg');
    expect(got).toHaveLength(total);
    expect(got[0]?.[1]).toBe(0);
    expect(got[total - 1]?.[1]).toBe((total - 1) % 4096);
    // Times must stay monotonic across every growth step.
    expect(got.every((p, i) => i === 0 || p[0] > (got[i - 1]?.[0] ?? 0))).toBe(true);
  });

  it('keeps everything since connect by default — that is the whole point of the live session', () => {
    const store = new RtStore(EDF_CHANNELS);
    for (let i = 0; i < 20_000; i += 1000) {
      store.append(samples({ ecg: { n0: i, v: Array.from({ length: 1000 }, () => 1) } }, i));
    }
    expect(seriesLength(store.source().ecg)).toBe(20_000);
  });

  it('drops the oldest points when a maxPoints ceiling is set', () => {
    const store = new RtStore(EDF_CHANNELS, { maxPoints: 500 });
    store.append(samples({ ecg: { n0: 0, v: Array.from({ length: 1200 }, (_, i) => i % 4096) } }));
    const got = pairs(store, 'ecg');
    expect(got).toHaveLength(500);
    expect(got[0]?.[1]).toBe(700); // the first 700 were trimmed off the front
    expect(got[499]?.[1]).toBe(1199);
  });

  it('declares every channel the device announced, so its pane exists before any sample lands', () => {
    const store = new RtStore(EDF_CHANNELS);
    const src = store.source();
    expect(Object.keys(src)).toHaveLength(11);
    expect(src.rr?.declared).toBe(true);
    expect(seriesLength(src.rr)).toBe(0);
  });

  it('ignores a block naming a channel the device never declared', () => {
    const store = new RtStore([channel('ecg')]);
    expect(() => { store.append(samples({ eeg: { n0: 0, v: [1, 2] } })); }).not.toThrow();
    expect(store.totalPoints).toBe(0);
  });
});
