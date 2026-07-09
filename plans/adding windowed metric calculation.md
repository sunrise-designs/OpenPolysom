# Windowed Clinical Metrics: brush-select a chart region → compute PLM/PLMI for just that window

## Context

Today the viewer only ever shows metrics (PLMI, HRV RMSSD, limb-move counts) computed once, over the **whole recording**, by `read_log.py`'s batch run and baked into `meta.json`/`events.json` at export time. There is no way to ask "what was the PLM index just during this 5-minute stretch I dragged over?" — every number on screen is a full-night aggregate.

The browser already has interactive window-selection working: `chart.ts`/`main.ts`'s ECharts **brush** (`brushType:'lineX'`) lets a user drag a range across the synced multi-pane chart, and `wireBrushSync` (`src_web/src/main.ts:107-128`) already computes a clean `[start_s, end_s]` (`BrushRange`, elapsed seconds) on every selection — currently used only to update a duration readout in `#sig-range`.

This plan wires that existing selection to a new **on-demand windowed-metrics compute service**: the browser POSTs the selected window to a small Python (later Rust) backend, which re-runs the same AASM PLM scoring (`src_python/signal_processing.py`) over just that slice of the recording — with careful padding so a windowed score means the same thing as the existing full-night score — and returns a number the viewer displays next to the chart. The user (owner of both the Python processing and the C++ ingest side) has decided this ships as a **new standalone FastAPI process**, separate from the existing static-file dev server, so it can later run as its own deployable service and be swapped for a Rust implementation without touching the browser side.

This is a different concern from decision **O10** (`wiki/state/decisions.md`) — the deferred raw-sample "slicing server" (`/window?start&end&channels&res` for decimated *chart rendering*). O10 is explicitly TS-only and about serving raw samples faster; this feature computes a **derived clinical number** on demand and is explicitly Python-now/Rust-later per the user. `wiki/state/decisions.md` needs a new entry recording this (call it **O12** as a placeholder — renumber to whatever's next when merged) so the split from O10 is on record, not just implied by this plan.

Architecture choices already settled with the user:
- **FastAPI**, not Flask or stdlib `http.server` — gives request validation, an auto-generated OpenAPI schema (the contract a future Rust service must satisfy), and async/cloud-friendly serving. First HTTP-framework dependency in the repo.
- **Separate process/port** from `export_zarr.py`'s existing `serve_and_open` static file server — matches how the site is actually deployed (Netlify serves statics; nothing today serves compute), and is the natural seam for a future Rust swap.
- **Test scope**: only the new modules get tests. `signal_processing.py` has zero tests today despite the coding standard calling for them — backfilling that is real but separate work, out of scope here.

---

## 1. Metric registry (Python) — the extensibility seam

New pure module, no I/O, following the repo's "functional core, imperative shell" standard:

**`src_python/metrics_registry.py`** (new)

```python
@dataclass(frozen=True)
class MetricSpec:
    metric: str                                    # 'plmi'
    channel_options: tuple[str, ...]                # ('accel_mag', 'accel1_mag', 'accel_combined_mag')
    zarr_arrays: Callable[[str], tuple[str, ...]]   # channel -> which Zarr arrays to read
    compute: Callable[[dict[str, np.ndarray], float, dict], dict]  # pure: arrays + fs + params -> result dict
    params_schema: dict[str, dict]                  # {'threshold': {'type': 'number', 'default': 8.0, 'min': 0}}
    context_before_s: float
    context_after_s: float
    min_window_s: float
    result_fields: tuple[str, ...]

METRICS: dict[str, MetricSpec] = {'plmi': PLMI_SPEC}
```

The one entry needed now — reuses `count_plm`/`combine_bilateral_vm` from `src_python/signal_processing.py:69,83` directly, no duplicated AASM logic:

```python
def _plmi_channels(channel: str) -> tuple[str, ...]:
    return {
        'accel_mag':          ('accel_x', 'accel_y', 'accel_z'),
        'accel1_mag':         ('accel1_x', 'accel1_y', 'accel1_z'),
        'accel_combined_mag': ('accel_x', 'accel_y', 'accel_z', 'accel1_x', 'accel1_y', 'accel1_z'),
    }[channel]

def _plmi_compute(arrays: dict[str, np.ndarray], fs: float, params: dict) -> dict:
    threshold = params.get('threshold', 8.0)
    if 'accel1_x' not in arrays:
        return count_plm(arrays['accel_x'], arrays['accel_y'], arrays['accel_z'], threshold=threshold, fs=fs)
    if 'accel_x' not in arrays:
        return count_plm(arrays['accel1_x'], arrays['accel1_y'], arrays['accel1_z'], threshold=threshold, fs=fs)
    r0 = count_plm(arrays['accel_x'], arrays['accel_y'], arrays['accel_z'], threshold=threshold, fs=fs)
    r1 = count_plm(arrays['accel1_x'], arrays['accel1_y'], arrays['accel1_z'], threshold=threshold, fs=fs)
    return combine_bilateral_vm(r0['vm'], r1['vm'], threshold=threshold, fs=fs)

PLMI_SPEC = MetricSpec(
    metric='plmi', channel_options=('accel_mag', 'accel1_mag', 'accel_combined_mag'),
    zarr_arrays=_plmi_channels, compute=_plmi_compute,
    params_schema={'threshold': {'type': 'number', 'default': 8.0, 'min': 0.0}},
    context_before_s=120.0, context_after_s=120.0, min_window_s=15.0,
    result_fields=('plmi', 'total_lms', 'total_plms', 'total_hours', 'lm_events', 'plm_groups'),
)
```

`channel` values reuse the vocabulary already established in `chart.ts` (`channelKey`) and `export_zarr.py`'s `_build_scoring` (`accel_mag`/`accel1_mag`/`accel_combined_mag`) rather than inventing a new one — one channel vocabulary across `events.json`, the viewer, and this endpoint.

Adding **windowed HRV** later (roadmap, not this change) is one more `MetricSpec` calling `compute_hrv` (`signal_processing.py:108`) with `context_before_s = context_after_s = 0.0` — RMSSD's successive-difference calc needs zero boundary padding, unlike PLM's median-filter + gap-rule scoring (see §3). This is the concrete shape the registry pattern is buying: new metrics are a new `MetricSpec` + registry entry, not new endpoint code.

---

## 2. Request/response schema

**Endpoint:** `POST /v1/recordings/{recording_id}/metrics`

`recording_id` resolves to `<recordings_root>/<recording_id>.zarr` + `<recordings_root>/<recording_id>_meta.json`, mirroring the stem convention `export_zarr.py:save_zarr_json` already uses (`stem.with_suffix('.zarr')` / `stem + '_meta.json'`). `recordings_root` is a config value (`PROTOSOM_RECORDINGS_ROOT` env var, default cwd) — the one pluggability seam for cloud storage later (§4).

Request:

```json
{
  "window": { "start_s": 1234.5, "end_s": 1420.0 },
  "metrics": [
    { "metric": "plmi", "channel": "accel_combined_mag", "params": { "threshold": 8.0 } }
  ]
}
```

Response:

```json
{
  "recording_id": "biometric_2026-07-08_01-57-52",
  "requested_window": { "start_s": 1234.5, "end_s": 1420.0 },
  "results": [
    {
      "metric": "plmi", "channel": "accel_combined_mag", "status": "ok",
      "params": { "threshold": 8.0, "fs": 50.0 },
      "window_used": {
        "start_s": 1234.5, "end_s": 1420.0,
        "context_before_s": 120.0, "context_after_s": 120.0,
        "padded_start_s": 1114.5, "padded_end_s": 1540.0,
        "clipped_to_recording_bounds": false
      },
      "value": { "plmi": 12.9, "total_lms": 4, "total_plms": 4, "total_hours": 0.05139 }
    }
  ],
  "provenance": { "git": { "sha": "92c5e57...", "dirty": false, "branch": "main" },
                  "service": "metrics_service", "computed_at": "2026-07-09T12:34:56Z" }
}
```

Per-metric error (so one bad metric in a batch doesn't kill the request): `{ "metric": "plmi", "channel": "accel_mag", "status": "error", "error": { "code": "window_too_short", "message": "window is 8.0s; plmi requires >= 15.0s" } }`. Whole-request failures use HTTP status: `404` unknown `recording_id`, `400` unknown `metric`/`channel`, `422` malformed window (`end_s <= start_s`, non-finite).

Naming follows the repo exactly: snake_case, `_s` suffix on every second-denominated field, `plmi`/`total_lms`/`total_plms`/`total_hours` reused verbatim from `types.ts`'s `Stats`/`LegStats`.

**PII:** request/response carry only `recording_id` + channel/metric names + numeric results — never the `subject`/`pii` block that lives in `meta.json` today.

**Provenance:** reuse `export_zarr._git_provenance()` — import it, don't reimplement. No result is written to disk; this response is the entire provenance record for an ephemeral computation.

---

## 3. Windowing / edge-effect strategy — what "windowed PLMI" means

Grounded in `src_python/signal_processing.py` (read in full):

**Two real sources of edge error if you naively slice `[start_s, end_s]` and run `count_plm` on just that:**
1. `remove_baseline`'s `median_filter(..., mode='reflect')` (`signal_processing.py:13`) needs ~`window/2` real samples of context each side to avoid reflect-padding artifacts (default `window_sec=30` → 15 s).
2. `_score_vm`'s PLM grouping (`signal_processing.py:38-55`) walks onset-to-onset gaps up to `MAX_GAP=90s`; a series whose first onset falls inside the window may depend on an LM up to 90 s *before* `start_s` to be correctly recognized as a continuing series rather than scored as a fresh, too-short one.

**Padding:** `context_before_s = context_after_s = 120.0` (covers both: 15 s baseline half-window + 90 s max PLM gap, rounded up). Read `[start_s − 120, end_s + 120]`, clipped to `[0, recording_duration_s]`, run the full `count_plm`/`combine_bilateral_vm` pipeline on the padded slice, then filter back to the requested window:

- `total_lms` (windowed) = LM events with `onset_s ∈ [start_s, end_s)`.
- `total_plms` (windowed) = LM events that are members of a **qualifying** PLM group (qualification determined using the full padded context) **and** have `onset_s ∈ [start_s, end_s)` — gives correct partial credit to a series straddling the boundary.
- `plmi` (windowed) = `total_plms / ((end_s − start_s) / 3600)` — the **requested** window's own duration, never the padded duration. This is the definition this plan commits to.

**Documented residual limitation, not silently fixed:** a real PLM series starting more than 120 s before `start_s` is still undercounted at the leading edge — AASM series length is theoretically unbounded (only each gap needs ≤90 s). `window_used.context_before_s/after_s` are echoed specifically so this is visible, not hidden. A test pins this as expected behavior (§8) so a future change can't silently "fix" it without the test/doc being updated deliberately.

**Metrics needing no padding:** a future windowed RMSSD slices RR beats directly to `[start_s, end_s]` — no boundary artifact analogous to PLM's median filter or gap rule. `context_before_s = context_after_s = 0.0` in its `MetricSpec`.

`min_window_s = 15.0` for PLM (~3×`MIN_DUR`) — below it, return `window_too_short` rather than a misleadingly-precise `plmi: 0.0` indistinguishable from "genuinely quiet."

---

## 4. Data access — the Zarr gap that must close

Pure/imperative split:

- **`src_python/metrics_windowing.py`** (new, pure) — `pad_window(start_s, end_s, context_before_s, context_after_s, recording_duration_s)`, `filter_lm_events(...)`, `filter_plm_group_members(...)`, `windowed_hours(start_s, end_s)`. No numpy/zarr imports — where the windowed-vs-full-night consistency property (§8) lives conceptually.
- **`src_python/metrics_zarr_reader.py`** (new, imperative shell) — `locate_recording(recordings_root, recording_id)`, `read_window(location, arrays, padded) -> dict[str, np.ndarray]` (opens the Zarr group via `zarr.open_group`, slices each array to the padded sample range — `zarr-python`, matching what `export_zarr.py` already uses server-side, not `zarrita`), `sample_rate(meta)`. **This is the cloud pluggability seam**: swapping local-filesystem `zarr.open_group` for an fsspec/S3-backed store is confined to this one module.

**The concrete gap:** `export_zarr.py:save_zarr_json` (confirmed at line 114 on read) writes `accel_x/y/z` for **Accel0 only** (lines 159-161); Accel1's raw axes are never written, only its already-scored `accel1_mag`. Without raw Accel1 axes, `metrics_zarr_reader` cannot recompute PLM for `accel1_mag`/`accel_combined_mag` over an arbitrary window — falling back to slicing the already-baseline-removed `accel1_mag` trace is wrong, because baseline removal isn't correctly re-runnable on an already-filtered trace for a new, smaller window.

**Required change:**
- `save_zarr_json` gains an optional parameter `accel1_raw: tuple[list, list, list] | None = None`. When provided, write three new arrays parallel to the existing `accel_x/y/z` block (same pattern as lines 159-161): `accel1_x`, `accel1_y`, `accel1_z`, `float32`, same single-chunk-per-array convention the rest of the file already uses (matching existing codec for internal consistency — the repo-wide Blosc(zstd,shuffle) migration per decision S4 is a separate, larger pre-existing gap this plan doesn't take on).
- `read_log.py`'s `main()` already computes `a1x, a1y, a1z` — build `raw1 = ([float(v) for v in a1x], [float(v) for v in a1y], [float(v) for v in a1z])` the same way `raw` is built for Accel0, and pass `accel1_raw=raw1` alongside the existing `accel1_mag=result1['vm']` call.
- Note for whoever maintains `wiki/knowledge/data-formats.md`/`architecture.md`: the array inventory should mention these two new arrays (doc update, not part of this change).

**Not in scope:** `src_web/src/zarr_loader.ts`/`types.ts` do not need to read `accel1_x/y/z` — computation stays server-side; the browser never computes metrics itself (keeps S6 intact: the TS side stays a pure Zarr/JSON reader plus this one new outbound metrics call).

**Known pre-existing performance ceiling, flagged not fixed:** every array (including the new ones) is single-chunk-per-array (`export_zarr.py` comment: "zarr 3.x has async overhead per chunk"), so a "windowed" read still decompresses the whole night before slicing in numpy. Harmless at PoC scale (a few hours of float32 accel data), but defeats "fast, not batch" at real scale. Follow-up (not this change): time-chunk `export_zarr.py`'s arrays so `metrics_zarr_reader.py` only touches chunks overlapping `[padded_start_s, padded_end_s]`.

---

## 5. TS wiring

**New files:**
- **`src_web/src/metrics_config.ts`** — resolves the metrics service base URL: (1) `?metrics=` query override, (2) `window.PROTOSOM_METRICS_URL` set by a new static `src_web/metrics-config.js` (loaded by `index.html` before `dist/chart.js`, same pattern as `styles.css`/`sw.js`/`manifest.webmanifest` already being static assets beside the bundle), (3) undefined → feature quietly unavailable, card doesn't render. Dev default: `window.PROTOSOM_METRICS_URL = 'http://localhost:8800';`. Decoupling the metrics origin from the viewer's origin matters because Netlify (today's deploy target per `deploy.py`) can't host a long-running Python process — the metrics service lives elsewhere in the cloud case.
- **`src_web/src/metrics_client.ts`** — `requestWindowedMetrics(baseUrl, recordingId, window: BrushRange, metrics): Promise<MetricsResponse>`, a thin `fetch` POST wrapper. This is the **first** outbound network call from the viewer (everything else today is `zarr_loader.ts` reads). Non-2xx and network errors resolve to a typed error result rather than throwing, so a metrics-service outage degrades the card to "unavailable" instead of breaking the chart.
- **`types.ts` additions**: `MetricWindow { readonly start_s: number; readonly end_s: number }`, `MetricRequestItem { readonly metric: string; readonly channel: string; readonly params?: Readonly<Record<string, number>> }`, `MetricResult`, `MetricsResponse` — mirroring the existing `Stats`/`LegStats` style.

**Wiring point:** `main.ts`'s `wireBrushSync` (lines 107-128) already computes a deduped `BrushRange` per selection (`lastRange` guard, `rangesEqual` at line 88) and currently only updates `#sig-range`'s text (lines 117-119). Add the metrics request call right there, passing `meta.recording_id` down (needs a new parameter on `wireBrushSync`, called from `run()` which already has `meta` loaded at line 186). Render results via direct DOM mutation into a new card (not a `renderShell` re-render — that would tear down the live ECharts instances).

**Where it renders:** a new **right-rail card** (`aside.right-rail`, `shell.ts:305-308`), not `#sig-range`. `#sig-range` is one line of toolbar text ("Drag on a chart to select a range" / a duration) — too small for a metric value + unit + threshold + per-channel breakdown. The right-rail already holds two structurally similar result cards (Montage, Spectrogram) built the same way (`card card-pad` + a builder function like `plmiGauge`/`kpiCards` in `shell.ts`); add a third, `windowMetricsCard()`, producing `<div class="card card-pad" id="window-metrics-card">…</div>`. Initial/empty state mirrors the `NO_RANGE_TEXT` pattern — export `NO_WINDOW_METRICS_TEXT = 'Select a range on a chart to compute PLMI for that window.'` from `shell.ts`. While a request is in flight (throttled by the brush's existing `throttleDelay: 200`, `chart.ts:129`), show a subtle "computing…" state; on a per-metric `status: 'error'`, show `error.message`; on transport failure, show "metrics service unavailable."

**Test:** `src_web/test/metrics_client.test.ts` — mock global `fetch` (vitest `vi.stubGlobal`), assert the POST body matches §2 exactly, assert both non-2xx and network-throw resolve to the typed error path (no uncaught rejection).

---

## 6. Process / dev workflow

- **`src_python/serve_metrics.py`** (new) — argparse: `--port` (default `8800`), `--host` (default `127.0.0.1`), `--recordings-root` (default cwd); `uvicorn.run(metrics_service.app, host=host, port=port)`. Mirrors `serve.py`'s existing CLI shape.
- **`src_python/metrics_service.py`** (new) — the FastAPI `app`; `CORSMiddleware` permissive (`allow_origins=["*"]`) for now — no auth/session/PII crosses this endpoint, and the static server's port is OS-assigned per run (`export_zarr.py` picks a free port), so a fixed allow-list isn't workable yet. Revisit once a real cloud origin exists.
- **`requirements.txt`** gains `fastapi`, `uvicorn[standard]` (pydantic ships with fastapi) — first HTTP-framework dependency in the repo.
- Run alongside the existing flow: `python src_python/serve_metrics.py` in one terminal, `python src_python/serve.py --meta <file>` (or `read_log.py -c`, which calls `serve_and_open`) in another. Two processes, two ports, as decided.
- Doc note for whoever maintains `how to use.md`: needs one added step pointing at `serve_metrics.py` as optional (chart works without it, just without the metrics card).

---

## 7. Cloud-deployability & Rust-swap-readiness

- HTTP JSON contract (§2) is the *only* coupling point — the TS side knows a base URL and this schema, nothing Python-specific.
- FastAPI's auto-generated `/openapi.json` becomes the spec a Rust replacement (axum/actix-web) must satisfy — worth snapshotting to a committed `src_python/metrics_openapi.json` so the contract is pinned independent of the Python implementation.
- Fully stateless request/response, no server-side session — any number of instances sit behind a load balancer trivially.
- The compute layer (`metrics_registry.py` + the `signal_processing.py` functions it wraps) is pure — the natural first candidate for a Rust port, independent of the FastAPI shell and `metrics_zarr_reader.py`'s storage access.
- Deployment substrate is separate from the static Netlify site by design (§5) — `deploy.py` only ships static assets; this service needs its own long-running host later.

---

## 8. Testing (new modules only, per the user's scoping decision)

- **`src_python/tests/test_metrics_windowing.py`** (new; add `pytest` to `requirements.txt`):
  - `pad_window` clips to `[0, recording_duration_s]` at the recording's edges.
  - `filter_lm_events`/`filter_plm_group_members` keep only `onset_s ∈ [start_s, end_s)`.
  - `windowed_hours` uses the requested span, never the padded span.
- **`src_python/tests/test_metrics_registry_plmi.py`** (new):
  - **Reference:** synthetic accel fixture, exactly 4 jerks 10 s apart, fully inside a window with ample padding both sides → windowed `total_plms`/`plmi` matches a direct `count_plm` call bit-for-bit.
  - **Consistency property:** request the whole recording as the window (`start_s=0, end_s=duration`) → windowed result equals `count_plm`/`combine_bilateral_vm`'s full-night result exactly. Regression guard that filtering logic doesn't silently change semantics when the window happens to be the whole night.
  - **Boundary/padding test:** a PLM series whose last onset lands just before `end_s` but whose offset extends past it — assert the LM is scored correctly using padded context but included by onset-based inclusion; a same-series LM whose *onset* falls after `end_s` is excluded.
  - **Known-limitation test:** a PLM series starting >120 s before `start_s` → assert the windowed result **does** undercount relative to a wider window — keeps the §3 residual limitation honest and tracked, not silently regressed.
  - **`window_too_short`:** a window shorter than `min_window_s` → per-metric `error` status, not `plmi: 0.0`.
- **`src_python/tests/test_metrics_service.py`** (new) — FastAPI `TestClient` against a small fixture recording (extend `tools/make_fixture.py`'s pattern): response schema shape, `404` unknown `recording_id`, `422` for `end_s <= start_s`, CORS header present.
- **`src_web/test/metrics_client.test.ts`** (new) — per §5.

---

## Critical files

- `src_python/signal_processing.py` — the pure DSP functions (`count_plm`, `combine_bilateral_vm`, `compute_hrv`) the registry wraps, never duplicates.
- `src_python/export_zarr.py` — `save_zarr_json` (needs `accel1_raw` param + two new arrays), `_git_provenance` (reused, not duplicated).
- `src_python/read_log.py` — one-line change to pass Accel1's raw axes through.
- `src_web/src/main.ts` — `wireBrushSync` (lines 107-128) is the exact wiring point for firing the metrics request on a stable brush selection.
- `src_web/src/shell.ts` — where the new right-rail metrics card and its empty-state text get added, alongside `types.ts` for the new request/response interfaces.
- `wiki/state/decisions.md` — needs a new entry recording this as distinct from O10.

## Verification

1. **Unit/property tests** — `pytest src_python/tests/` (new suite, §8) green, including the consistency property (windowed-whole-night == full-night) and the documented-limitation test.
2. **TS unit test** — `npm test` in `src_web/` picks up `metrics_client.test.ts`.
3. **End-to-end manual check**: run `read_log.py` on a fixture recording to produce a Zarr + meta.json with both accelerometers' raw axes; start `serve_metrics.py` and `serve.py`/`read_log.py -c`'s `serve_and_open`; open the viewer, drag-select a range on an Accel pane, confirm the right-rail card shows a PLMI value; verify selecting the *entire* recording's span produces the same PLMI shown in the hero gauge (the consistency property, checked live); kill `serve_metrics.py` and confirm the card degrades to "metrics service unavailable" instead of breaking the chart.
