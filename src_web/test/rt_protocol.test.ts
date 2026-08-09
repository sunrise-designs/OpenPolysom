import { describe, it, expect } from 'vitest';
import { affineMap, gapBefore, isCompatibleProtocol, parseRtMessage } from '../src/rt_protocol';
import { RT_PROTOCOL } from '../src/rt_types';
import { EDF_CHANNELS, channel, helloFixture } from './helpers/rtFixtures';

const frame = (obj: unknown): string => JSON.stringify(obj);

describe('parseRtMessage — hello', () => {
  it('accepts the device\'s full 11-channel declaration', () => {
    const msg = parseRtMessage(frame(helloFixture()));
    expect(msg.type).toBe('hello');
    if (msg.type !== 'hello') return;
    expect(msg.channels).toHaveLength(11);
    expect(msg.channels.map((c) => c.name)).toEqual([
      'thoracic', 'abdomen', 'flow', 'ecg',
      'accel0_x', 'accel0_y', 'accel0_z',
      'accel1_x', 'accel1_y', 'accel1_z', 'rr',
    ]);
    expect(msg.device_uid).toBe('A1B2C3D4E5F6');
  });

  it('rejects a stream whose protocol major differs, rather than mis-decoding it', () => {
    const msg = parseRtMessage(frame(helloFixture({ protocol: 'protosom.rt/2.0.0' })));
    expect(msg.type).toBe('error');
  });

  it('accepts a newer minor version — later revisions only add optional fields', () => {
    expect(isCompatibleProtocol('protosom.rt/1.7.3')).toBe(true);
    expect(isCompatibleProtocol('protosom.rt/2.0.0')).toBe(false);
    expect(isCompatibleProtocol('something.else/1.0.0')).toBe(false);
    expect(parseRtMessage(frame(helloFixture({ protocol: 'protosom.rt/1.4.0' }))).type).toBe('hello');
  });

  it('rejects a channel with a zero-width digital range — it has no map back to physical units', () => {
    const broken = helloFixture({
      channels: [{ ...channel('ecg'), digital_min: 100, digital_max: 100 }],
    });
    expect(parseRtMessage(frame(broken)).type).toBe('error');
  });

  it('rejects a hello with no channels at all', () => {
    expect(parseRtMessage(frame(helloFixture({ channels: [] }))).type).toBe('error');
  });
});

describe('parseRtMessage — samples', () => {
  const samples = {
    type: 'samples',
    seq: 412,
    blocks: {
      ecg: { n0: 12840, v: [2043, 2051, 2088] },
      thoracic: { n0: 6420, v: [812, 809] },
    },
  };

  it('parses a frame and keeps each block\'s absolute sample index', () => {
    const msg = parseRtMessage(frame(samples));
    expect(msg.type).toBe('samples');
    if (msg.type !== 'samples') return;
    expect(msg.seq).toBe(412);
    expect(msg.blocks.ecg?.n0).toBe(12840);
    expect(msg.blocks.ecg?.v).toEqual([2043, 2051, 2088]);
    expect(msg.blocks.thoracic?.n0).toBe(6420);
  });

  it('accepts an empty frame — a 100 ms window holds no 2.5 Hz RR sample most of the time', () => {
    const msg = parseRtMessage(frame({ type: 'samples', seq: 1, blocks: {} }));
    expect(msg.type).toBe('samples');
  });
});

describe('parseRtMessage — malformed input is dropped, never thrown', () => {
  const bad = [
    '',
    'not json at all',
    '[1,2,3]',
    'null',
    frame({ type: 'samples', seq: 1 }),                                    // no blocks
    frame({ type: 'samples', blocks: {} }),                                // no seq
    frame({ type: 'samples', seq: 1, blocks: { ecg: { n0: -1, v: [1] } } }), // negative index
    frame({ type: 'samples', seq: 1, blocks: { ecg: { n0: 1.5, v: [1] } } }), // fractional index
    frame({ type: 'samples', seq: 1, blocks: { ecg: { n0: 0, v: ['x'] } } }), // non-numeric sample
    frame({ type: 'samples', seq: 1, blocks: { ecg: { n0: 0 } } }),         // no values
    frame({ type: 'wat' }),
    frame({ noTypeAtAll: true }),
  ];

  it.each(bad)('returns an error for %j instead of throwing', (raw) => {
    expect(() => parseRtMessage(raw)).not.toThrow();
    expect(parseRtMessage(raw).type).toBe('error');
  });

  it('rejects a non-finite sample value — NaN is the gap marker, it must not arrive on the wire', () => {
    // JSON has no NaN literal, so a producer emitting one produces invalid JSON;
    // this pins the behaviour if a lenient parser ever lets it through.
    expect(parseRtMessage('{"type":"samples","seq":1,"blocks":{"ecg":{"n0":0,"v":[NaN]}}}').type).toBe('error');
  });
});

describe('parseRtMessage — status', () => {
  it('parses device health and treats a missing battery as unknown, not zero', () => {
    const msg = parseRtMessage(frame({
      type: 'status', recording: true, elapsed_s: 128, sd_error: false,
      dropped_frames: 3, clients: 1,
    }));
    expect(msg.type).toBe('status');
    if (msg.type !== 'status') return;
    expect(msg.recording).toBe(true);
    expect(msg.elapsed_s).toBe(128);
    expect(msg.dropped_frames).toBe(3);
    expect(msg.batt_pct).toBeNull();
  });
});

describe('affineMap — the EDF+ digital→physical scaling, per logger.cpp SigDef', () => {
  it('is the identity for ECG, whose physical and digital ranges coincide', () => {
    const map = affineMap(channel('ecg'));
    expect(map(0)).toBe(0);
    expect(map(2048)).toBe(2048);
    expect(map(4095)).toBe(4095);
  });

  it('is the identity for RR (0–2000 ms over 0–2000 digital)', () => {
    const map = affineMap(channel('rr'));
    expect(map(790)).toBe(790);
  });

  it('scales thoracic by 2e6/65534 with no offset — the symmetric ±32767 range', () => {
    const map = affineMap(channel('thoracic'));
    expect(map(0)).toBeCloseTo(0, 9);
    expect(map(1)).toBeCloseTo(2e6 / 65534, 6);
    expect(map(32767)).toBeCloseTo(1e6, 6);
    expect(map(-32767)).toBeCloseTo(-1e6, 6);
  });

  it('scales flow to ±100 mbar across the same symmetric range', () => {
    const map = affineMap(channel('flow'));
    expect(map(32767)).toBeCloseTo(100, 9);
    expect(map(-32767)).toBeCloseTo(-100, 9);
    expect(map(0)).toBeCloseTo(0, 9);
  });

  it('carries the accelerometers\' asymmetric −8192/8191 range through to a non-zero offset', () => {
    // The MMA8451's digital range is asymmetric, so unlike the RIP/flow channels
    // the map does NOT pass through the origin. Getting this wrong would bias
    // every accelerometer trace by ~0.06 mg — small, but wrong, and invisible
    // without an assertion like this one.
    const map = affineMap(channel('accel0_x'));
    const gain = 4000 / 16383;
    expect(map(8191)).toBeCloseTo(2000, 6);
    expect(map(-8192)).toBeCloseTo(-2000, 6);
    expect(map(0)).toBeCloseTo(-2000 + gain * 8192, 9);
    expect(map(0)).not.toBe(0);
  });

  it('maps every declared channel\'s digital endpoints onto its physical endpoints', () => {
    for (const ch of EDF_CHANNELS) {
      const map = affineMap(ch);
      expect(map(ch.digital_min)).toBeCloseTo(ch.physical_min, 6);
      expect(map(ch.digital_max)).toBeCloseTo(ch.physical_max, 6);
    }
  });
});

describe('gapBefore', () => {
  it('counts the samples missing between what we hold and what arrived', () => {
    expect(gapBefore(100, 100)).toBe(0);
    expect(gapBefore(100, 105)).toBe(5);
  });

  it('reports no gap for a retransmit or an overlapping block', () => {
    // The store re-anchors on n0 and skips the duplicate prefix, so an overlap
    // must not be drawn as a break in the line.
    expect(gapBefore(100, 95)).toBe(0);
  });
});
