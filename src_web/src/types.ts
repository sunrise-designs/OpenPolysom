export interface PatientInfo {
  name: string;
  dob: string;
  nhs_number: string;
  email: string;
}

export interface RecordingMeta {
  date: string;
  legs: string;
  start_time: string;
  end_time: string;
}

export interface Stats {
  total_lms: number;
  total_plms: number;
  plmi: number;
  total_hours: number;
  hrv_overall: number | null;
  threshold: number | null;
  window_sec: number | null;
  fs: number;
}

export interface SidecarMeta {
  patient: PatientInfo | null;
  recording: RecordingMeta | null;
  stats: Stats;
  lm_events: [number, number][];
  plm_groups: [number, number][][];
  git_hash: string;
  zarr_path: string;
}

/** Decoded time series loaded from the Zarr store. */
export interface ZarrData {
  t: Float32Array;
  rr: Float32Array;
  accel_x: Uint8Array;
  accel_y: Uint8Array;
  accel_z: Uint8Array;
  accel_mag: Float32Array;
  hrv_t: Float32Array;
  hrv_rmssd: Float32Array;
}
