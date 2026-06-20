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

export interface Stats {
  readonly plmi: number;
  readonly total_lms: number;
  readonly total_plms: number;
  readonly total_hours: number;
  readonly hrv_rmssd_overall: number | null;
  readonly threshold: number;
  readonly window_sec: number;
  readonly fs: number;
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
    readonly biosignals: ReadonlyArray<{
      readonly path: string;
      readonly format: string;
      readonly hash: Hash;
    }>;
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
  readonly channels?: ReadonlyArray<string>;
}

export interface EventGroup {
  readonly id: string;
  readonly type: string;
  readonly member_ids: ReadonlyArray<string>;
  readonly onset_s: number;
  readonly duration_s: number;
  readonly params?: Readonly<Record<string, unknown>>;
}

export interface Scoring {
  readonly scoring_id: string;
  readonly events: ReadonlyArray<Event>;
  readonly groups: ReadonlyArray<EventGroup>;
}

export interface EventsDoc {
  readonly schema: string;
  readonly schema_version: string;
  readonly recording_id: string;
  readonly scorings: ReadonlyArray<Scoring>;
}

// ── Zarr working store (decoded) ─────────────────────────────────────────────

/** Decoded dense signals from the working store. */
export interface ZarrData {
  readonly t: Float64Array;
  readonly accel_x: Uint8Array;
  readonly accel_y: Uint8Array;
  readonly accel_z: Uint8Array;
  readonly accel_mag: Float32Array;
  readonly rr: Float32Array;
  readonly hrv_t: Float64Array;
  readonly hrv_rmssd: Float32Array;
}
