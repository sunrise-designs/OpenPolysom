import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { parseRtMessage } from '../src/rt_protocol';
import { RT_PROTOCOL } from '../src/rt_types';
import type { RtChannelDef } from '../src/rt_types';
import { EDF_CHANNELS } from './helpers/rtFixtures';

/**
 * The cross-language contract check for the live stream, in the spirit of
 * wiki/standards/coding.md § "Cross-language read parity".
 *
 * Three descriptions of the same thirteen channels have to agree, and they live
 * in three different languages and repos-worth of code:
 *
 *   1. `logger.cpp`'s `SigDef` table — what is written to the EDF+ raw anchor.
 *   2. `rt_stream.c`'s `HELLO_CHANNELS` — what the device announces on the wire.
 *   3. `test/helpers/rtFixtures.ts` — what this viewer's tests assert against.
 *
 * If they drift, the viewer silently mis-scales a signal: the affine map comes
 * from (2), the samples come from (1), and nothing at runtime would notice. So
 * the firmware sources are read and compared here rather than trusted.
 */

const REPO = resolve(__dirname, '../..');
const read = (rel: string): string => readFileSync(resolve(REPO, rel), 'utf8');

/** Parse the `SigDef` rows out of logger.cpp: {label, transducer, dim, rate, dmax, dmin, pmax, pmin}. */
function parseSigDef(source: string): readonly RtChannelDef[] {
  const table = /static const SigDef sigs\[NUM_SIGNALS\] = \{([\s\S]*?)\n\s*\};/.exec(source);
  if (table === null) throw new Error('SigDef table not found in logger.cpp');

  const row = /\{\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*([0-9.]+)\s*,\s*(-?[0-9.e+]+)\s*,\s*(-?[0-9.e+]+)\s*,\s*(-?[0-9.e+]+)\s*,\s*(-?[0-9.e+]+)\s*\}/g;
  return Array.from(table[1].matchAll(row), (m) => ({
    name: '', // logger.cpp knows EDF labels, not canonical names — matched on edf_label below
    edf_label: m[1],
    transducer: m[2],
    unit: m[3],
    sample_rate_hz: Number(m[4]),
    digital_max: Number(m[5]),
    digital_min: Number(m[6]),
    physical_max: Number(m[7]),
    physical_min: Number(m[8]),
  }));
}

/** Parse `HELLO_CHANNELS` out of rt_stream.c by reassembling its concatenated string literals. */
function parseHelloChannels(source: string): readonly RtChannelDef[] {
  // `\r?` because a Windows checkout (core.autocrlf) has CRLF line endings.
  const decl = /static const char \*HELLO_CHANNELS =([\s\S]*?);\r?\n/.exec(source);
  if (decl === null) throw new Error('HELLO_CHANNELS not found in rt_stream.c');

  const json = Array.from(decl[1].matchAll(/"((?:[^"\\]|\\.)*)"/g), (m) => m[1])
    .join('')
    .replace(/\\"/g, '"');

  // Round-trip through the real client parser, so this also proves the device's
  // hand-written JSON is something the viewer will actually accept.
  const msg = parseRtMessage(JSON.stringify({
    type: 'hello', protocol: RT_PROTOCOL, device_uid: 'X', recording_start_iso: '',
    timezone: '', recording_active: true, record_duration_s: 10,
    channels: JSON.parse(json) as unknown,
  }));
  if (msg.type !== 'hello') throw new Error(`device channel table rejected by the client parser: ${msg.type === 'error' ? msg.reason : ''}`);
  return msg.channels;
}

const sigDef = parseSigDef(read('ESP32-C6-heart-idf/components/logger/logger.cpp'));
const hello = parseHelloChannels(read('ESP32-C6-heart-idf/components/rt_stream/rt_stream.c'));

describe('the live stream announces exactly the channels the EDF+ records', () => {
  it('finds all thirteen signals in logger.cpp and in rt_stream.c', () => {
    expect(sigDef).toHaveLength(13);
    expect(hello).toHaveLength(13);
    expect(EDF_CHANNELS).toHaveLength(13);
  });

  it('streams the channels in EDF signal order', () => {
    expect(hello.map((c) => c.edf_label)).toEqual(sigDef.map((c) => c.edf_label));
  });

  it.each(EDF_CHANNELS.map((c) => c.name))('%s matches logger.cpp\'s SigDef row', (name) => {
    const fixture = EDF_CHANNELS.find((c) => c.name === name);
    const streamed = hello.find((c) => c.name === name);
    const recorded = sigDef.find((c) => c.edf_label === fixture?.edf_label);

    expect(streamed).toBeDefined();
    expect(recorded).toBeDefined();
    if (fixture === undefined || streamed === undefined || recorded === undefined) return;

    // The scaling fields are the ones that silently corrupt a trace if they
    // drift, so they are compared against the firmware's own EDF header values.
    for (const key of ['sample_rate_hz', 'digital_min', 'digital_max', 'physical_min', 'physical_max'] as const) {
      expect(streamed[key], `${name}.${key} (stream vs EDF+)`).toBe(recorded[key]);
      expect(fixture[key], `${name}.${key} (test fixture vs EDF+)`).toBe(recorded[key]);
    }
    expect(streamed.edf_label).toBe(recorded.edf_label);
    expect(streamed.transducer).toBe(recorded.transducer);
    expect(streamed.unit).toBe(recorded.unit);
    expect(streamed.no_reading_digital, `${name}.no_reading_digital (stream vs fixture)`).toBe(fixture.no_reading_digital);
  });
});

describe('the temperature no-reading marker is the one logger.h writes', () => {
  // TEMP_DIGITAL_NO_READING is what logger_temp_digital() writes for a missing
  // reading; the stream must announce that same value, or the viewer would plot
  // a dropout as -200 °C. It must also be inside the declared digital range —
  // edflib clamps every sample into it, so an out-of-range marker would arrive
  // as digital_min anyway.
  const header = read('ESP32-C6-heart-idf/components/logger/include/logger.h');
  const max = Number(/#define TEMP_DIGITAL_MAX\s+(\d+)/.exec(header)?.[1]);
  const marker = /#define TEMP_DIGITAL_NO_READING\s+\(-TEMP_DIGITAL_MAX\)/.test(header) ? -max : Number.NaN;

  it.each(['temp0', 'temp1'])('%s announces TEMP_DIGITAL_NO_READING, inside its digital range', (name) => {
    const ch = hello.find((c) => c.name === name);
    expect(ch?.no_reading_digital).toBe(marker);
    expect(ch?.digital_min).toBe(marker);
    expect(ch?.digital_max).toBe(max);
  });
});

describe('the firmware and the viewer agree on the protocol version', () => {
  it('rt_stream.c announces the version this client speaks', () => {
    const source = read('ESP32-C6-heart-idf/components/rt_stream/rt_stream.c');
    expect(source).toContain(`\\"protocol\\":\\"${RT_PROTOCOL}\\"`);
  });
});
