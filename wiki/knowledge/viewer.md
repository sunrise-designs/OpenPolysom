---
title: The TS Web App (Viewer)
domain: knowledge
status: living
updated: 2026-08-09
summary: The TS web app has two modes — batch, which reads the Zarr boundary plus metadata and displays it (synced multi-pane charts, channel-filtered event markArea overlays, a brush-selected windowed-metrics card, a landing page listing every deployed study), and RT, which plots the eleven raw EDF+ channels live from the device's WebSocket — never writing Zarr in either.
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

The prototype already does the round-trip end to end against the wrist-device recording.
The pieces:

| File | Role |
| --- | --- |
| `src_web/src/main.ts` | Entry. Reads `?meta=`, loads `meta.json` + Zarr, inits one ECharts canvas instance, wires window resize. |
| `src_web/src/zarr_loader.ts` | Fetches `meta.json` and opens each named Zarr array (`t`, `rr`, `accel_x/y/z`, `accel_mag`, `hrv_t`, `hrv_rmssd`), plus `accel1_mag`/`accel_combined_mag` when present — read via `readOptionalArray`, which degrades to an empty array rather than failing the load when a store has only one accelerometer. |
| `src_web/src/chart.ts` | Builds the ECharts option: synced multi-pane grid, LM/PLM `markArea` overlays, dataZoom, crosshair tooltip, title strip. |
| `src_web/src/types.ts` | `SidecarMeta` (patient / recording / stats / `lm_events` / `plm_groups` / `git_hash` / `zarr_path`) and `ZarrData`. |
| `src_web/src/signals.ts` | `SignalSource` — canonical channel name → series. The one data surface `chart.ts` reads, produced by both modes. |
| `src_web/src/rt_types.ts` · `rt_protocol.ts` | The live stream's types, and its pure half: frame parsing, the EDF+ affine map, gap arithmetic. |
| `src_web/src/rt_client.ts` · `rt_store.ts` · `rt_config.ts` | The live stream's I/O half: reconnecting WebSocket, growable sample buffers, and the `?rt=` / `PROTOSOM_RT_URL` resolution. |

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
- **Event markArea overlays, filtered per channel.** `events.json` carries one `scorings[]` entry per
  scored channel — Accel0's own LM/PLM series, Accel1's own, and the combined bilateral series (see
  [signal processing](signal-processing.md) §3) — with every event/group tagged `channels: [<zarr array
  name>]`. `chart.ts`'s `overlayMarkArea`/`collectSpans` filter on that tag, so the Accel0 mag pane only
  shows Accel0's own spans, the Accel1 mag pane only Accel1's, and the combined pane only the combined
  series' — never all three at once. Translucent green for LMs, red/teal-bordered dashed for PLM series.
  An event/group with no `channels` tag (older single-accelerometer stores, e.g.
  `tools/make_fixture.py`'s sample) is treated as belonging to `accel_mag` — the one channel that has
  always implicitly meant "the" accelerometer — so legacy fixtures keep working unchanged.
- **Accel1 + combined bilateral panes, optional.** `chart.ts`'s `CHANNELS` list has two trailing entries —
  `Accel1 mag (leg 2)` (`accel1_mag`) and `Combined LM (bilateral)` (`accel_combined_mag`) — marked
  `optional: true`: `channels()` drops them from the rendered set (and from the Montage rail) whenever
  their backing Zarr array is empty, i.e. whenever only one accelerometer was scored. `channelMeta(zarr)`
  (data-aware, replacing a static list) drives both the `sig-grid` card count and the Montage rail row
  count in `shell.ts`'s `renderShell`, so the two representations never disagree about how many panes
  exist for a given recording.
- **Pane context menu — "Zoom to window".** Right-click (desktop) or a 500 ms long-press (touch) on
  any pane opens a small DOM menu (`main.ts`'s `wireContextMenu`, styled `.ctx-menu`) whose
  "Zoom to window" item zooms **every** connected pane to the band last brush-selected —
  `chart.ts`'s pure `zoomToRangeAction` builds the `dataZoom` payload in axis values (elapsed
  seconds), and `echarts.connect` mirrors the single dispatch across the group. Sub-second
  selections are widened to a 1 s floor rather than collapsing the axis. The brush cursor is armed
  at init on desktop but **on demand on touch** (the menu's "Select a window" item), because an
  always-on brush there would turn every vertical page-scroll swipe over a pane into a selection —
  the same canvas-swallows-the-gesture problem `wireCtrlZoom` documents for the wheel.
- **Large-line path.** Each series sets `large: true`, `largeThreshold`, and `sampling: 'lttb'` — the
  documented ECharts way to draw long lines on canvas without choking (`chart.ts:123-165`).
- **Title strip** (`chart.ts:22-40`, `buildTitle`) renders patient / recording / stats and the `git_hash`
  provenance stamp.

> PII (Personally Identifiable Information) note: `types.ts` still carries a `patient` block (name / DOB / NHS), and `buildTitle` renders it
> inline (`chart.ts:24-25`). Per the PII decision in [decisions](../state/decisions.md), PII stays in a
> separable block and the [clinical export](data-formats.md) scrubs the EDF+ header; the committed sample
> data is anonymous (accel + RR, no identifiers). As the slicing server lands, the title's PII becomes
> opt-in rather than always-on.

## RT vs batch — the viewer's two modes

The viewer has two ways in, chosen in `main.ts`'s router:

| Entry | Mode | Source |
| --- | --- | --- |
| `index.html?meta=…` | **batch** | the [Zarr working store](data-formats.md) + `meta.json` + `events.json` — a finished, processed recording |
| `index.html?rt=<host or ws URL>` | **RT** | a live WebSocket from the device's `rt_stream` component — samples as they are acquired |
| neither | landing | `studies.json` |

RT exists because nothing about a night's montage is checkable until the night is
over: a belt that came unplugged or an electrode that lifted is currently discovered
the next morning. It is a **second read path, not a second boundary** — nothing is
persisted, nothing derived is computed, the EDF+ on the SD card is still the raw
anchor, and the viewer still never writes Zarr. See [decisions § S12](../state/decisions.md).

### What RT has and does not have

Everything downstream of [Python processing](signal-processing.md) is **absent**, not
faked: no PLMI gauge, no KPI row, no narrative, no LM/PLM `markArea` overlays, no
windowed-metrics card, no provenance footer. The live shell says so explicitly in an
"After the night" card rather than rendering empty widgets. The channels are the
**eleven raw EDF+ channels** — including `ecg` and the `accel1_*` axes, which no
derived store carries — and none of the derived ones (`accel_mag`, `hrv_rmssd`,
`accel_combined_mag`), which need baseline removal and beat extraction that stay in
Python. What RT adds is a live status strip: link state, recording state, battery,
and the three separate loss counters (device-dropped frames, unparseable frames,
drawn gaps).

Everything *chart*-shaped is shared: `chart.ts`'s `CHANNELS` table, the option
builders, `echarts.connect` sync, dataZoom, the brush, the context menu. Both modes
produce a `SignalSource` (`src_web/src/signals.ts`) — canonical channel name → series —
so there is no mode branch below `main.ts`. Batch supplies it via
`zarr_loader.ts`'s `toSignalSource`; RT via `rt_store.ts`.

### The wire protocol (`protosom.rt/1.0.0`)

JSON text frames, ~10 per second. Channel names are the canonical snake_case names
from [zarr-schema-spec § 3.2](../planning/zarr-schema-spec.md), so the two modes
cannot disagree about what a channel is.

- **`hello`**, once on connect: device UID, recording start, and a `channels[]` array
  that is `logger.cpp`'s `SigDef` table verbatim — label, transducer, unit, rate, and
  the digital/physical min-max pair. The stream is self-describing; the viewer builds
  its panes and its scaling from this rather than hardcoding eleven channels.
- **`samples`**, repeated: `{seq, blocks: {<channel>: {n0, v[]}}}`. Values are the
  **digital integers `edfwrite_digital_samples` receives** — one streamed point per
  EDF+ sample, bit-identical to what reaches the card, because `rt_stream` calls
  `logger.cpp`'s own conversion functions rather than re-deriving them. `n0` is the
  **absolute** sample index since recording start, so sample *i* sits at
  `(n0 + i) / sample_rate_hz` — the same elapsed second it occupies in the EDF+, and
  self-anchoring after a dropped frame or a reconnect.
- **`status`**, ~1 Hz: recording state, elapsed, SD error, dropped frames, clients.

The client applies the EDF+ affine map (`rt_protocol.ts`'s `affineMap`) — the same
arithmetic `edf_reader.py` performs host-side. A block whose `n0` skips ahead gets a
single `NaN` point, so ECharts draws a **break** rather than interpolating across
samples that never arrived. Malformed frames are counted and dropped, never fatal.

### Reading the whole session

RT keeps **everything since connect**, so a night is scrollable end to end. Each
channel is one growable interleaved `[t, v, …]` `Float64Array` (`rt_store.ts`), handed
to ECharts as a zero-copy `subarray` — `chart.ts`'s `zip()` would allocate a JS array
object per point, which is fine for a loaded recording and ruinous for millions of
streamed samples. `chart.appendData()` is *not* used: it cannot grow a coordinate
system's extent, and a live axis does nothing but grow. Instead the render tick
re-sets the buffer, on a period that scales with its size (`main.ts`'s `liveTickMs`):
10 Hz early on, easing to 0.5 Hz deep into a night, keeping the fraction of time spent
redrawing roughly flat. `RtStore`'s `maxPoints` option is the escape hatch if a
session ever needs a memory ceiling instead.

### Running it without hardware

`src_web/tools/mock_rt_server.mjs` replays a recorded `.edf` over the identical
protocol at wall-clock rate (`--speed`, `--start`, `--drop` for exercising gap
rendering). It has no dependencies — the ~40 lines of RFC 6455 needed to serve text
frames are inlined. Because it replays a real recording rather than synthesising
waveforms, it is also how the round-trip is checked: a sample plotted at elapsed *t*
must be the sample at index *t*·rate in the file.

`test/rt_firmware_contract.test.ts` closes the loop the other way — it reads
`logger.cpp`'s `SigDef` and `rt_stream.c`'s `HELLO_CHANNELS` out of the firmware
sources and asserts all eleven channels agree with each other and with the viewer's
fixtures. Drift there would silently mis-scale a trace, and nothing at runtime would
notice.

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

## Multi-study hosting: the landing page and `deploy.py`

The site ([`src_python/deploy.py`](../../src_python/deploy.py), Netlify) hosts **many
studies at once**, not one recording per site. The site root (`index.html` with no
`?meta=` param) is a **landing page** — `src_web/src/landing.ts`'s `renderLanding`
lists every deployed study as a card (subject, PLMI, limb moves, HRV, recorded date),
each linking to `index.html?meta=studies/<recording_id>/meta.json` — the same viewer
entry point as before, just pointed at a per-study path instead of the site root.

- **Layout.** The shared viewer shell (`index.html`, `dist/chart.js`, `styles.css`,
  `sw.js`, `manifest.webmanifest`, `metrics-config.js`, icons) lives at the site
  root, common to every study. Each study's `meta.json` / `events.json` / Zarr
  store live under `studies/<recording_id>/`. `main.ts` resolves `events.json` and
  the Zarr path relative to whichever `meta.json` was loaded, so this nesting
  needed **no viewer changes** beyond adding the landing branch itself.
  `deploy.py`'s `_STATIC_ASSETS` list is the single place these shared files are
  enumerated — it has no way to discover a new un-bundled static asset on its
  own, so a `<script src=...>`/`<link href=...>` added to `index.html` (like
  `metrics-config.js` was) must be added there too, or it silently 404s live.
- **`studies.json`** — a site-root manifest (`StudySummary[]` in `types.ts`) the
  landing page fetches to build its list. `deploy.py` maintains it: it reads the
  *live* site's current `studies.json` (not local state), replaces any existing entry
  for the recording being deployed, appends/updates the new one, and re-uploads it.
- **Incremental deploys (why re-deploying is cheap).** Netlify's deploy API accepts a
  manifest of `{path: sha1}` and returns only the digests it doesn't already have —
  everything else is assumed unchanged and is **not re-uploaded**. `deploy.py` fetches
  the site's previously-published file list (`GET /deploys/{id}/files`) and folds it
  into the new manifest unmodified, then adds only the shared assets (recomputed
  fresh, in case the chart bundle changed) and the one study actually being deployed.
  So deploying study N+1 uploads roughly "the shared assets if they changed" +
  "study N+1's own files" — **not** every prior study's data — which is the literal
  saving referenced by "save hosting tokens" (fewer bytes shipped, fewer upload
  calls). See [decisions](../state/decisions.md) if this pattern needs to generalise
  (e.g. a slicing server fronting many studies) rather than stay a `deploy.py` detail.

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

### Windowed clinical metrics — a separate, already-built service (not the slicing server below)

Distinct from the deferred raw-sample slicing server (O10, below): a brush-drag selection on any
chart (`chart.ts`'s `brushOption`, wired in `main.ts`'s `wireBrushSync`) POSTs the selected
`[start_s, end_s]` window to a small standalone **FastAPI** service
([`src_python/metrics_service.py`](../../src_python/metrics_service.py), run via
[`serve_metrics.py`](../../src_python/serve_metrics.py) on its own port — see
[decisions § S10](../state/decisions.md)) and renders a computed metric (currently windowed PLMI)
into a right-rail card (`shell.ts`'s `windowMetricsCard`). This is a *derived clinical number*
computed on demand, not a decimated-sample fetch for rendering — the two features solve different
problems and are **not** the same server. The metric-registry pattern
([`metrics_registry.py`](../../src_python/metrics_registry.py)) is the extensibility seam for
future windowed metrics (HRV, apnea, …); [`metrics_windowing.py`](../../src_python/metrics_windowing.py)
documents the boundary-padding strategy AASM-style scoring needs so a windowed score means the same
thing as the existing full-night score. The service is quietly unavailable (no card shown) when
`window.PROTOSOM_METRICS_URL` isn't configured (`src_web/metrics-config.js`) — the chart works fine
without it.

### The windowed API (raw-sample slicing server — deferred, O10)

When the slicing server exists, the viewer talks to it over a small HTTP windowed API:

- `GET /window?start&end&channels&res` — decimated samples for the named channels over the visible range, at
  screen resolution. `channels` is a comma list matching the Zarr array names (e.g.
  `thoracic,abdomen,flow,ecg`, or `accel0_x,accel0_y,accel0_z`). `res` is the target point count;
  the server never returns more points than the window can show.
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
