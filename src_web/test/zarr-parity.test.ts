import { describe, it, expect } from 'vitest';
import * as zarr from 'zarrita';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { FsStore } from './helpers/fsStore';

const here = dirname(fileURLToPath(import.meta.url));
const fixture = join(here, 'fixtures', 'parity.zarr');

/**
 * Cross-language read parity: the fixture is written by zarr-python with
 * Blosc(zstd, shuffle), filters=null, dimension_separator=".". This asserts
 * zarrita.js decodes it byte-correctly — the regression guard for the
 * "Zarr v2 + Blosc, no Python-only filters" boundary rule.
 */
describe('Zarr boundary read parity (zarr-python -> zarrita.js)', () => {
  it('decodes a Blosc(zstd,shuffle) float32 array including the NaN fill path', async () => {
    const store = new FsStore(fixture);
    const arrNode = await zarr.open(zarr.root(store).resolve('f32'), { kind: 'array' });
    const chunk = await zarr.get(arrNode as zarr.Array<zarr.NumberDataType>);
    const out = Array.from(chunk.data);

    expect(out.length).toBe(8);
    expect(out.slice(0, 3)).toEqual([0, 1.5, 2.5]);
    expect(out.filter((x) => Number.isNaN(x)).length).toBe(1);
    expect(out[7]).toBe(7);
  });

  it('decodes a Blosc(zstd,shuffle) uint8 array', async () => {
    const store = new FsStore(fixture);
    const arrNode = await zarr.open(zarr.root(store).resolve('u8'), { kind: 'array' });
    const chunk = await zarr.get(arrNode as zarr.Array<zarr.NumberDataType>);

    expect(Array.from(chunk.data)).toEqual([10, 20, 30, 40, 50, 60, 70, 80]);
  });
});
