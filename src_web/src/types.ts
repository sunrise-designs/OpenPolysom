/**
 * Types for the ProtoSom Zarr boundary (reader side).
 *
 * These mirror the `meta.json` + `events.json` sidecars and the Zarr working
 * store written by the fixture generator (`tools/make_fixture.py`) — the
 * language-neutral contract. The TS web app only ever READS these.
 */

// ── meta.json ────────────────────────────────────────────────────────────────

export interface Subject {
  readonly subject_id: string;
  readonly sex: string | null;
  readonly dob: string | null;
}

export interface Recording {
  readonly start_iso: string;
  readonly duration_s: number;
  readonly n_samples: number;
  readonly sample_rate_hz: number;
  readonly source: string;
}

export interface LegStats {
  readonly plmi: number;
  readonly total_lms: number;
  readonly total_plms: number;
  readonly total_hours: number;
}

export interface Stats {
  /** Combined bilateral score (either leg) when two accelerometers were scored, else the single accelerometer's own score — see `legs` for the per-leg breakdown. */
  readonly plmi: number;
  readonly total_lms: number;
  readonly total_plms: number;
  readonly total_hours: number;
  readonly hrv_rmssd_overall: number | null;
  readonly threshold: number;
  readonly window_sec: number;
  readonly fs: number;
  /** Present only when a second (Accel1) accelerometer was scored alongside Accel0. */
  readonly legs?: { readonly accel0: LegStats; readonly accel1: LegStats };
}

export interface GitProvenance {
  readonly sha: string;
  readonly dirty: boolean;
  readonly branch: string;
}

export interface Hash {
  readonly algorithm: string;
  readonly value: string;
}

export interface Provenance {
  readonly generated_at: string;
  readonly pipeline: { readonly repo: string; readonly git: GitProvenance };
  readonly input_preparation: { readonly skip_samples: number; readonly note: string };
}

export interface Layers {
  readonly raw: {
    readonly biosignals: readonly {
      readonly path: string;
      readonly format: string;
      readonly hash: Hash;
    }[];
  };
  readonly working: { readonly path: string; readonly zarr_format: number };
}

export interface Meta {
  readonly schema_versions: Readonly<Record<string, string>>;
  readonly recording_id: string;
  readonly subject: Subject;
  readonly recording: Recording;
  readonly stats: Stats;
  readonly layers: Layers;
  readonly provenance: Provenance;
}

// ── events.json ──────────────────────────────────────────────────────────────

export interface Event {
  readonly id: string;
  readonly type: string;
  readonly onset_s: number;
  readonly duration_s: number;
  readonly channels?: readonly string[];
}

export interface EventGroup {
  readonly id: string;
  readonly type: string;
  readonly member_ids: readonly string[];
  readonly onset_s: number;
  readonly duration_s: number;
  readonly channels?: readonly string[];
  readonly params?: Readonly<Record<string, unknown>>;
}

export interface Scoring {
  readonly scoring_id: string;
  readonly events: readonly Event[];
  readonly groups: readonly EventGroup[];
}

export interface EventsDoc {
  readonly schema: string;
  readonly schema_version: string;
  readonly recording_id: string;
  readonly scorings: readonly Scoring[];
}

// ── studies.json (landing page) ──────────────────────────────────────────────

/** One entry in the site-wide `studies.json` manifest — enough to list and link a study without fetching its full `meta.json`. */
export interface StudySummary {
  readonly recording_id: string;
  /** Path to this study's `meta.json`, relative to the site root — pass as `?meta=` to open it. */
  readonly meta_path: string;
  readonly subject_id: string;
  readonly start_iso: string;
  readonly duration_s: number;
  readonly plmi: number;
  readonly total_lms: number;
  readonly hrv_rmssd_overall: number | null;
  /** When this study was deployed (ISO), not when it was recorded. */
  readonly deployed_at: string;
}

// ── Zarr working store (decoded) ─────────────────────────────────────────────

/** Decoded dense signals from the working store. */
export interface ZarrData {
  readonly t: Float64Array;
  readonly accel_x: Float32Array;
  readonly accel_y: Float32Array;
  readonly accel_z: Float32Array;
  readonly accel_mag: Float32Array;
  /** Accel1 (second/leg-2 accelerometer) vector magnitude. Empty when only one accelerometer was scored. */
  readonly accel1_mag: Float32Array;
  /** Bilateral combined (either-leg envelope) vector magnitude. Empty when only one accelerometer was scored. */
  readonly accel_combined_mag: Float32Array;
  readonly rr: Float32Array;
  readonly rr_t: Float64Array;
  readonly hrv_t: Float64Array;
  readonly hrv_rmssd: Float32Array;
}
