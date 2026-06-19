import { openArray, HTTPStore, NestedArray } from 'zarr';
import type { TypedArray } from 'zarr';
import type { ZarrData, SidecarMeta } from './types';

export async function loadMeta(metaUrl: string): Promise<SidecarMeta> {
  const resp = await fetch(metaUrl);
  if (!resp.ok) throw new Error(`Failed to fetch meta: ${metaUrl} (${resp.status})`);
  return resp.json() as Promise<SidecarMeta>;
}

async function loadTypedArray<T extends TypedArray>(store: HTTPStore, name: string): Promise<T> {
  const arr = await openArray({ store, path: name, mode: 'r' });
  const result = await arr.get();
  if (typeof result === 'number') throw new Error(`Expected array at '${name}', got scalar`);
  return (result as NestedArray<T>).data;
}

/** Fetch all Zarr arrays for a recording, given the base URL of the .zarr directory. */
export async function loadZarr(zarrBaseUrl: string): Promise<ZarrData> {
  const store = new HTTPStore(zarrBaseUrl);

  const [t, rr, accel_x, accel_y, accel_z, accel_mag, hrv_t, hrv_rmssd] = await Promise.all([
    loadTypedArray<Float32Array>(store, 't'),
    loadTypedArray<Float32Array>(store, 'rr'),
    loadTypedArray<Uint8Array>(store, 'accel_x'),
    loadTypedArray<Uint8Array>(store, 'accel_y'),
    loadTypedArray<Uint8Array>(store, 'accel_z'),
    loadTypedArray<Float32Array>(store, 'accel_mag'),
    loadTypedArray<Float32Array>(store, 'hrv_t'),
    loadTypedArray<Float32Array>(store, 'hrv_rmssd'),
  ]);

  return { t, rr, accel_x, accel_y, accel_z, accel_mag, hrv_t, hrv_rmssd };
}
