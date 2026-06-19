# Plan: Python → Zarr + JSON → TypeScript ECharts Viewer

## Context
`plotting_echarts.py` generates a self-contained HTML file by embedding all chart logic as Python f-strings containing JavaScript. The goal is to cleanly separate:
- **Data generation** (Python): parse `.bin` → signal processing → Zarr + JSON sidecar
- **Visualization** (TypeScript): a standalone web page served by a local HTTP server that ingests both files and renders ECharts

The Zarr format is the **stable schema contract** between the two sides — both Python (writer) and TypeScript (reader) evolve around it. This also prepares the pipeline for eventual cloud deployment (S3/GCS serve Zarr via HTTP range requests natively; the viewer JS is unchanged).

## Zarr Schema (the contract)

Format: **Zarr v2** (best JS library support via `zarr.js`).

```
{stem}.zarr/
  .zgroup                  — root group metadata
  t/                       — time axis (float32, seconds since recording start)
  rr/                      — RR interval series (float32, ms)
  accel_x/                 — raw accelerometer X (uint8, 0–255)
  accel_y/                 — raw accelerometer Y (uint8, 0–255)
  accel_z/                 — raw accelerometer Z (uint8, 0–255)
  accel_mag/               — baseline-removed vector magnitude (float32)
  hrv_t/                   — HRV sliding-window time axis (float32, seconds)
  hrv_rmssd/               — HRV RMSSD per window (float32, ms)
```

All arrays share the same time base (`t`). `hrv_t` and `hrv_rmssd` are shorter (one value per 5-min window).

## JSON Sidecar Schema (`{stem}_meta.json`)

Stores everything that is not a regular time series — sparse event lists, scalars, and patient/recording metadata. The JSON sidecar is the second half of the contract.

```json
{
  "patient": { "name": "...", "dob": "...", "nhs_number": "...", "email": "..." },
  "recording": { "date": "...", "legs": "...", "start_time": "...", "end_time": "..." },
  "stats": {
    "total_lms": 0,
    "total_plms": 0,
    "plmi": 0.0,
    "total_hours": 0.0,
    "hrv_overall": 0.0,
    "threshold": 8.0,
    "window_sec": 30.0,
    "fs": 10
  },
  "lm_events": [[onset_s, offset_s], ...],
  "plm_groups": [[[onset_s, offset_s], ...], ...],
  "git_hash": "abc1234",
  "zarr_path": "biometric.zarr"
}
```

## Architecture

### Python side

**New `src_python/export_zarr.py`**
- `save_zarr_json(stem, t, rr, accel_raw, accel_mag, hrv_t, hrv_rmssd, stats, recording_meta)` → writes `{stem}.zarr/` and `{stem}_meta.json`
- Reads `patient.json` internally (mirrors `_load_patient()` in `plotting_html.py`)
- Reads git hash internally (mirrors `_git_short_hash()`)

**Modify `src_python/read_log.py`**
- Replace the `save_echarts_html(...)` call with `save_zarr_json(...)`
- After writing, start `http.server` on a random available port in a background thread, then open `http://localhost:{port}/index.html` in the browser
- Index.html and chart.js live alongside the output files (or a fixed `src_web/dist/` path is served)

**Modify `src_python/requirements.txt`** — add `zarr` (v2.x, not v3)

### Web side (`src_web/`)

TypeScript project. Users never need npm to *use* it — `dist/chart.js` is pre-built and committed.

```
src_web/
  index.html          — HTML shell: chart container + title div
  src/
    main.ts           — entry: fetches meta JSON → fetches Zarr → calls buildChart()
    chart.ts          — ECharts option builder (clean TS port of current inline JS)
    zarr_loader.ts    — typed wrapper around zarr.js: loadRecording(metaUrl) → ZarrData
    types.ts          — interfaces: RecordingMeta, PatientInfo, Stats, ZarrData
  dist/
    chart.js          — pre-built bundle (esbuild, committed to repo)
  package.json        — dev dependency only: zarr, esbuild, typescript
  tsconfig.json
  build.sh            — one-liner: npx esbuild src/main.ts --bundle --outfile=dist/chart.js
```

`zarr_loader.ts` is the TypeScript mirror of the schema above — if the schema changes, this file changes with it.

## Files to create/modify

| File | Action |
|---|---|
| `src_python/export_zarr.py` | **New** — Zarr + JSON writer |
| `src_python/read_log.py` | Modify — swap `save_echarts_html` → `save_zarr_json`; add local server launch |
| `src_python/requirements.txt` | Add `zarr` |
| `src_python/plotting_echarts.py` | **Delete** (replaced) |
| `src_web/index.html` | **New** |
| `src_web/src/main.ts` | **New** |
| `src_web/src/chart.ts` | **New** |
| `src_web/src/zarr_loader.ts` | **New** |
| `src_web/src/types.ts` | **New** |
| `src_web/dist/chart.js` | **New** — pre-built, committed |
| `src_web/package.json` | **New** |
| `src_web/tsconfig.json` | **New** |
| `src_web/build.sh` | **New** |

## Workflow after this change

```
python read_log.py -f biometric.bin -c
# → writes biometric.zarr/  +  biometric_meta.json
# → starts http.server on random port, serving src_web/dist/ + output dir
# → opens http://localhost:PORT/index.html?meta=biometric_meta.json
```

Future cloud: upload `.zarr/` + `_meta.json` to S3 → `index.html?meta=https://...` — viewer JS unchanged.

## Verification
1. `python read_log.py -f biometric.bin -c` → check `biometric.zarr/` and `biometric_meta.json` created with correct arrays
2. Browser opens automatically → ECharts renders all subplots (RR, accel mag, HRV, raw channels)
3. Pan/zoom syncs across subplots; LM/PLM markArea annotations appear
4. Patient name, NHS number, PLM stats visible in title bar
5. `python -c "import zarr; z = zarr.open('biometric.zarr'); print(dict(z.arrays()))"` shows all 8 arrays
