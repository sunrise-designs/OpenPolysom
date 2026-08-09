import * as zarr from 'zarrita';
import type { Meta, EventsDoc, StudySummary, ZarrData } from './types';
import type { SignalSource } from './signals';

/** Fetch + parse a JSON sidecar. */
async function loadJson<T>(url: string): Promise<T> {
  const resp = await fetch(url);
  if (!resp.ok) throw new Error(`Failed to fetch ${url} (${String(resp.status)})`);
  return (await resp.json()) as T;
}

export const loadMeta = (url: string): Promise<Meta> => loadJson<Meta>(url);
export const loadEvents = (url: string): Promise<EventsDoc> => loadJson<EventsDoc>(url);

/**
 * Fetch the site-wide studies manifest for the landing page. Absence (a fresh
 * site with nothing deployed yet, or a local `index.html` opened with no
 * `studies.json` beside it) degrades to an empty list rather than an error.
 */
export async function loadStudies(url: string): Promise<readonly StudySummary[]> {
  const resp = await fetch(url);
  if (!resp.ok) return [];
  return (await resp.json()) as readonly StudySummary[];
}

/** Browser store: read the Zarr boundary directly over HTTP. */
export const httpStore = (baseUrl: string): zarr.FetchStore => new zarr.FetchStore(baseUrl);

/** Read one whole 1-D array from the working store. */
async function readArray(root: zarr.Location<zarr.Readable>, name: string): Promise<zarr.TypedArray<zarr.NumberDataType>> {
  const arr = await zarr.open(root.resolve(name), { kind: 'array' });
  const chunk = await zarr.get(arr as zarr.Array<zarr.NumberDataType>);
  return chunk.data;
}

/**
 * Read an array that may not exist in this store — e.g. `accel1_mag` /
 * `accel_combined_mag` are only written when a second accelerometer was
 * scored (see wiki/knowledge/signal-processing.md). Absence degrades to an
 * empty array rather than failing the whole load, matching how empty
 * `hrv_t`/`hrv_rmssd` already signal "not computed this recording".
 */
async function readOptionalArray(root: zarr.Location<zarr.Readable>, name: string): Promise<zarr.TypedArray<zarr.NumberDataType>> {
  try {
    return await readArray(root, name);
  } catch {
    return new Float32Array(0);
  }
}

/**
 * Decode the dense signals from the working store via zarrita (Zarr v2 + Blosc,
 * decoded out of the box). The store is injected so the browser passes a
 * `FetchStore` and tests pass a filesystem-backed store.
 */
export async function loadZarr(store: zarr.Readable): Promise<ZarrData> {
  const root = zarr.root(store);
  const [t, accelX, accelY, accelZ, accelMag, accel1Mag, accelCombinedMag, thoracic, abdomen, flow, rr, rrT, hrvT, hrvRmssd] = await Promise.all([
    readArray(root, 't'),
    readArray(root, 'accel_x'),
    readArray(root, 'accel_y'),
    readArray(root, 'accel_z'),
    readArray(root, 'accel_mag'),
    readOptionalArray(root, 'accel1_mag'),
    readOptionalArray(root, 'accel_combined_mag'),
    readOptionalArray(root, 'thoracic'),
    readOptionalArray(root, 'abdomen'),
    readOptionalArray(root, 'flow'),
    readArray(root, 'rr'),
    readArray(root, 'rr_t'),
    readOptionalArray(root, 'hrv_t'),
    readOptionalArray(root, 'hrv_rmssd'),
  ]);
  return {
    t: t as Float64Array,
    accel_x: accelX as Float32Array,
    accel_y: accelY as Float32Array,
    accel_z: accelZ as Float32Array,
    accel_mag: accelMag as Float32Array,
    accel1_mag: accel1Mag as Float32Array,
    accel_combined_mag: accelCombinedMag as Float32Array,
    thoracic: thoracic as Float32Array,
    abdomen: abdomen as Float32Array,
    flow: flow as Float32Array,
    rr: rr as Float32Array,
    rr_t: rrT as Float64Array,
    hrv_t: hrvT as Float64Array,
    hrv_rmssd: hrvRmssd as Float32Array,
  };
}

/**
 * Adapt the decoded working store onto the canonical channel names `chart.ts`
 * looks up (wiki/planning/zarr-schema-spec.md §3.2) — the same names the live
 * stream uses, so one channel table serves both modes.
 *
 * Two renames happen here rather than in the store: the derived layer writes
 * Accel0's raw axes as `accel_x/y/z` (from when there was only one
 * accelerometer), which the canonical registry calls `accel0_x/y/z`; and the
 * HRV series is keyed on its value array `hrv_rmssd`, with `hrv_t` becoming its
 * x.
 *
 * The `declared` split reproduces the two classes the channel table used to
 * hardcode. The first six are what `export_zarr.py` always writes, so their
 * panes render even when a particular recording left one empty (an HRV pane
 * with no beats still belongs on screen — it is a measurement that came back
 * empty, not a channel the device lacks). The rest are `readOptionalArray`
 * reads that are simply absent on some devices, and stay hidden when empty:
 * that is how a wrist-only recording keeps hiding the respiratory panes and a
 * single-accelerometer one keeps hiding leg 2.
 */
export function toSignalSource(z: ZarrData): SignalSource {
  return {
    rr: { x: z.rr_t, y: z.rr, declared: true },
    accel_mag: { x: z.t, y: z.accel_mag, declared: true },
    hrv_rmssd: { x: z.hrv_t, y: z.hrv_rmssd, declared: true },
    accel0_x: { x: z.t, y: z.accel_x, declared: true },
    accel0_y: { x: z.t, y: z.accel_y, declared: true },
    accel0_z: { x: z.t, y: z.accel_z, declared: true },
    thoracic: { x: z.t, y: z.thoracic },
    abdomen: { x: z.t, y: z.abdomen },
    flow: { x: z.t, y: z.flow },
    accel1_mag: { x: z.t, y: z.accel1_mag },
    accel_combined_mag: { x: z.t, y: z.accel_combined_mag },
  };
}
