---
title: The TS Web App (Viewer)
domain: knowledge
status: living
updated: 2026-06-19
summary: The TS web app reads the Zarr boundary plus metadata and displays it — synced multi-pane charts, event markArea overlays, and an audio spectrogram — never writing Zarr.
---

# The TS Web App (Viewer)

The viewer is **the TS web app** — Leon's side of the [architecture](architecture.md). It is the fourth and
final pipeline stage (device → [C++ ingest](hardware.md) → [Python processing](signal-processing.md) → the TS web
app) and the only one a human looks at directly. Its job is narrow and absolute: **read the
[Zarr boundary](data-formats.md) plus its JSON metadata and display it.** It is a read-only consumer of the
working store's derived layer; it **NEVER writes Zarr**.

On demand it may invoke C++ binaries (ingest, [clinical export](data-formats.md)) or trigger
[Python processing](signal-processing.md) re-runs as subprocesses — but those subprocesses write the derived layer,
not the web app. The raw anchor stays immutable; every derived product is stamped with provenance. See
[decisions](../state/decisions.md) for the authoritative three-language split: **C++ ingests, Python
processes, TypeScript presents; the three meet at the Zarr store + metadata.**

## What the viewer reads

The cross-language contract is **Zarr v2 + Blosc (zstd, shuffle)**, with no Python-only numcodecs filters —
the full spec is in [data formats](data-formats.md). The viewer reads two things from the working store:

- **The derived Zarr group** — one array per dense signal, chunked along time. Read via `zarrita.js` (see
  [the zarr.js → zarrita.js migration](#migrate-zarrjs--zarritajs) below).
- **The JSON sidecars** — `meta.json` (metadata + provenance) and `events.json` (sparse annotations: LM /
  PLM groups now; apnea/hypopnea, snore VOTE events later). The current sidecar shape is `SidecarMeta` in
  `src_web/src/types.ts`.

`main.ts` treats `meta.json` as the entry point: it takes a `?meta=` query param (`main.ts:6-15`), resolves
the recording's `zarr_path` relative to it, then loads the arrays (`main.ts:18-26`).

## Current `src_web/` (starting point)

The prototype already does the round-trip end to end against the legacy ESP32-S3 wrist-device recording.
The pieces:

| File | Role |
| --- | --- |
| `src_web/src/main.ts` | Entry. Reads `?meta=`, loads `meta.json` + Zarr, inits one ECharts canvas instance, wires window resize. |
| `src_web/src/zarr_loader.ts` | Fetches `meta.json` and opens each named Zarr array (`t`, `rr`, `accel_x/y/z`, `accel_mag`, `hrv_t`, `hrv_rmssd`). |
| `src_web/src/chart.ts` | Builds the ECharts option: synced multi-pane grid, LM/PLM `markArea` overlays, dataZoom, crosshair tooltip, title strip. |
| `src_web/src/types.ts` | `SidecarMeta` (patient / recording / stats / `lm_events` / `plm_groups` / `git_hash` / `zarr_path`) and `ZarrData`. |

Concrete behaviours worth noting (PoC realities, not the target spec):

- **Decimation lives in the viewer today.** `chart.ts` step-compresses RR to transition points only
  (`chart.ts:43-53`) and strides accel to every 3rd sample, ~3.3 Hz (`chart.ts:55-63`). This is on-the-fly
  client-side display reduction — fine at PoC scale, and the seed of the server-side decimation under
  [load-all-now / window-later](#load-all-now--window-later).
- **Synced multi-pane.** One row (ECharts `grid` + `xAxis` + `yAxis`) per signal, heights computed from the
  channel count (`chart.ts:74-107`). A single `dataZoom` pair — `'inside'` (scroll/drag) + `'slider'`
  (overview bar) — spans **all** x-axes via `zIdxs` (`chart.ts:167`, `181-184`), so panning/zooming one pane
  moves them all in lockstep. `filterMode: 'none'` keeps the full series in memory and just rescales the
  view.
- **Event markArea overlays.** LM events and PLM groups from `meta.lm_events` / `meta.plm_groups` render as
  ECharts `markArea` bands on the accel pane (`chart.ts:109-121`, `139`) — translucent green for LMs,
  red-bordered for PLM groups. As `events.json` grows (apnea/hypopnea, snore VOTE), the same `markArea`
  mechanism extends to more bands on the relevant panes.
- **Large-line path.** Each series sets `large: true`, `largeThreshold`, and `sampling: 'lttb'` — the
  documented ECharts way to draw long lines on canvas without choking (`chart.ts:123-165`).
- **Title strip** (`chart.ts:22-40`, `buildTitle`) renders patient / recording / stats and the `git_hash`
  provenance stamp.

> PII note: `types.ts` still carries a `patient` block (name / DOB / NHS), and `buildTitle` renders it
> inline (`chart.ts:24-25`). Per the PII decision in [decisions](../state/decisions.md), PII stays in a
> separable block and the [clinical export](data-formats.md) scrubs the EDF+ header; the committed sample
> data is anonymous (accel + RR, no identifiers). As the slicing server lands, the title's PII becomes
> opt-in rather than always-on.

## Migrate zarr.js → zarrita.js

`zarr_loader.ts` currently imports from `zarr` (zarr.js: `openArray`, `HTTPStore`, `NestedArray` —
`zarr_loader.ts:1-2`). **zarr.js is unmaintained.** The boundary spec mandates `zarrita.js` as the TS
reader (see [data formats](data-formats.md) and [decisions](../state/decisions.md)). The migration is
contained to `zarr_loader.ts`:

- Replace `HTTPStore` with a zarrita `FetchStore` pointed at the `.zarr` base URL.
- Replace `openArray(...).get()` with `zarrita.open(store, ...)` + `zarrita.get(arr)`, returning the typed
  `data` buffer.
- Keep `loadZarr` returning the same `ZarrData` shape so `chart.ts` and `main.ts` are untouched.
- zarrita supports **partial chunk reads** natively — which is what makes the windowed API below cheap to
  build on the same reading code.

## Charting: ECharts vs uPlot

- **ECharts** — works today (canvas renderer + LTTB `sampling: 'lttb'`, `large` / `largeThreshold` per
  series). Gives the synced multi-pane grid, `dataZoom`, `markArea` events, and crosshair tooltip out of the
  box. This is the current default.
- **uPlot** (MIT, ~50 KB) — optional, much lighter, very fast for plain time series. A candidate if/when
  ECharts bundle size or render cost becomes a constraint. Either way the chart consumes arrays of
  `[time, value]` points; the charting library is replaceable without touching the Zarr boundary or the
  slicing server. Treat the choice as an [open fork](../state/decisions.md).

The two charting concerns the viewer must keep: **synced panes** (one `dataZoom` driving every x-axis) and
**event markArea bands** keyed off `events.json`.

## Audio spectrogram pane

Audio is a separate pane, not mixed into the biosignal grid. The raw anchor for audio is a time-anchored
**FLAC** sidecar (see [data formats](data-formats.md)); the viewer never decodes FLAC or runs an FFT.
Instead [Python processing](signal-processing.md) **pre-computes the spectrogram array** (and the snore VOTE / MFCC
features) and stores the spectrogram in the derived Zarr; the viewer reads that 2-D array and renders it
with **wavesurfer.js** in its own pane, time-aligned to the biosignal panes' shared x-axis and the same
`dataZoom`. Keeping the FFT on the Python side keeps the viewer a pure display layer and keeps the
spectrogram reproducible/auditable.

## Load-all-now / window-later

The viewer's reading code is written once and **moved, not rewritten**, as data grows:

- **PoC scale (now): browser-direct.** A whole recording is tiny, so `main.ts` loads every array in full and
  ECharts holds the lot in memory (`filterMode: 'none'`). No server.
- **As data grows: a thin TS slicing server.** The same zarrita reading code moves from the browser into a
  small TS server (Node, or a single binary via Bun / Deno / Node SEA — see [packaging](../planning/packaging.md)). The
  server reads the Zarr and serves the browser only **the data the chart needs**: screen-resolution windows
  for the visible range, not the whole recording. It decimates on the fly (the client-side striding in
  `chart.ts` moves server-side); precomputed pyramids can be added later **without changing the viewer**.

Same module on both sides of the move — "read Zarr, hand back the visible window" — which is why the
boundary contract is language-neutral and why the viewer never needs to know whether it is reading the
filesystem or an HTTP endpoint. Explicitly: pyramids are part of the derived layer, so they are written by
[Python processing](signal-processing.md), never by the viewer; they change only how the slicing server answers
`/window`, not the API and not the chart.

### The windowed API

When the slicing server exists, the viewer talks to it over a small HTTP windowed API:

- `GET /window?start&end&channels&res` — decimated samples for the named channels over the visible range, at
  screen resolution. `channels` is a comma list matching the Zarr array names (e.g.
  `Thoracic,Abdomen,Flow,HR_Raw` for RPi5 data, or `accel_x,accel_y,accel_z,rr` for the ESP32-S3 wrist
  device). `res` is the target point count; the server never returns more points than the window can show.
- `GET /spectrogram?start&end` — the Python-precomputed spectrogram slice for the audio pane.
- `GET /meta` — `meta.json` (metadata + provenance), with PII attached only when explicitly requested.
- `GET /events` — `events.json` sparse annotations for the `markArea` overlays.

The payload is intentionally dumb: arrays of numbers + timestamps, ready to feed straight into a chart
series. No Zarr semantics, no chunk metadata, no codec knowledge leaks past the boundary.

## What the viewer must never do

- **Never write Zarr.** Writing the raw layer is [C++ ingest](hardware.md); writing the derived layer is
  [Python processing](signal-processing.md). The viewer reads only.
- **Never compute clinical features in the browser.** AASM PLM/LM, HRV, apnea/hypopnea, snore VOTE
  classification all live in [Python processing](signal-processing.md) and arrive via `meta.json` / `events.json`.
  The transition-compression and striding in `chart.ts` are display decimation, not feature extraction.
- **Never mutate the raw anchor.** On-demand C++/Python subprocesses regenerate the derived layer with fresh
  provenance; layer 1 stays immutable.

## Related

- [architecture](architecture.md) — the four-stage pipeline and three-layer data model.
- [data formats](data-formats.md) — the Zarr v2 + Blosc boundary spec, JSON sidecars, EDF+/FLAC raw anchors.
- [ingest](hardware.md) — C++, writes the raw Zarr layer.
- [processing](signal-processing.md) — Python, writes the derived Zarr + `meta.json` + `events.json`, pre-computes the spectrogram.
- [packaging](../planning/packaging.md) — bundling the TS web app + thin slicing server (Node / Bun / Deno SEA), air-gapped.
- [decisions](../state/decisions.md) — settled decisions and open forks (PDF report, ECharts vs uPlot, when pyramids land).
