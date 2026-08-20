# Repo root cleanup: pipeline outputs → `data_scratchpad/`

## Context

The repo root has accumulated generated artifacts. Every run of the analysis pipeline
writes its outputs into the **current working directory**, and `how to use.md` tells the
user to run everything from the repo root — so each recording leaves a
`biometric_<timestamp>.zarr/` store, a `biometric_<timestamp>_meta.json` sidecar and an
`events.json` behind. There are currently 5 `.zarr` dirs (~34 MB), 4 `_meta.json` files
and one `events.json` sitting in the root, plus a stray `node_modules/` and two
gitignored build caches (`.quartz/`, `public/`).

Two consequences:

1. **Root bloat** — generated data is indistinguishable from source at a glance.
2. **A real privacy leak.** `biometric_2026-07-08_01-57-52_meta.json` and
   `biometric_2026-07-09_18-15-01_meta.json` are **tracked** and contain a
   `subject.pii` block with real name, DOB, NHS number and email. They are committed
   in `92c5e57`, `3b168e6` and `3219565` on a public repo. `.gitignore` only excludes
   the exact names `biometric_meta.json` / `biometric_filtered_meta.json`, so
   timestamped outputs were never covered.
   [wiki/standards/privacy.md:15](../../../../Repos/ProtoSom/wiki/standards/privacy.md#L15)
   asserts "No real PII is, or ever was, committed" — that claim is wrong and must be
   corrected.

Intended outcome: pipeline outputs land in a single gitignored `data_scratchpad/`
folder, PII-bearing files are untracked and can no longer be committed by accident, and
the root loses the docker + build-cache clutter.

**Decided scope** (user's call): untrack the PII files at HEAD only — **no history
rewrite**. The blobs stay reachable in the public repo's history; that exposure is
handled separately, outside this plan.

---

## 1. Pipeline writes to `data_scratchpad/`

### `src_python/read_log.py`

- Add `--out-dir` (default `data_scratchpad`) to the argparse block (~line 45, next to
  `--chart`).
- At [read_log.py:129](../../../../Repos/ProtoSom/src_python/read_log.py#L129), pass
  `Path(args.out_dir) / src_path.stem` as the `stem` argument to `save_zarr_json`
  instead of the bare `src_path.stem`. `save_zarr_json` already treats `stem` as a
  path (`Path(stem).with_suffix('.zarr')`), so no signature change is needed.
- The "To view:" hint at line 142 already interpolates `meta_path`, so it picks up the
  new directory automatically — no separate string to keep in sync.
- Same for the non-echarts branch if `save_plotly_html` writes to CWD — check
  `plotting_html.py` and route its output through the same `--out-dir`.

### `src_python/export_zarr.py`

- In `save_zarr_json` (~line 155, right after `stem = Path(stem)`), add
  `stem.parent.mkdir(parents=True, exist_ok=True)` so the directory is created on
  first use. `events_path` is already derived as `meta_path.parent / 'events.json'`, so
  it follows automatically.
- Add a module-level `DEFAULT_OUT_DIR = Path('data_scratchpad')` here — `export_zarr`
  is the module both `serve.py` and `deploy.py` already import from, so it is the
  natural single source of truth for the folder name.

### `src_python/serve.py`

- [serve.py:17](../../../../Repos/ProtoSom/src_python/serve.py#L17): auto-discovery
  globs `Path.cwd()`. Search `DEFAULT_OUT_DIR` first, then fall back to `Path.cwd()`
  so an existing `--stem`/bare-root layout still resolves.
- Line 15 (`Path(args.stem + '_meta.json')`) should resolve the stem relative to
  `DEFAULT_OUT_DIR` when the bare name does not exist in CWD.

### `src_python/deploy.py`

- `_resolve_paths` ([deploy.py:194-212](../../../../Repos/ProtoSom/src_python/deploy.py#L194-L212))
  hardcodes `cwd = Path.cwd()` in all three branches. Apply the same
  "`data_scratchpad` first, CWD fallback" rule, and update the error message and the
  `epilog` examples (lines 218-221) to show the new paths.

### `src_python/serve_metrics.py` / `metrics_service.py`

- `--recordings-root` default `'.'` → `'data_scratchpad'`
  ([serve_metrics.py:9](../../../../Repos/ProtoSom/src_python/serve_metrics.py#L9)).
- `metrics_service.RECORDINGS_ROOT`'s env fallback `'.'` → `'data_scratchpad'`
  ([metrics_service.py:35](../../../../Repos/ProtoSom/src_python/metrics_service.py#L35)).
- `src_python/tests/test_metrics_service.py:41` monkeypatches `RECORDINGS_ROOT` to a
  `tmp_path`, so tests are unaffected.

### `how to use.md`

- Step 2's example command stays as-is (input path unchanged), but add a note that
  outputs now land in `data_scratchpad/` and that the folder is gitignored.
- Step 3's example becomes
  `python src_python/serve.py --meta data_scratchpad/biometric_2026-07-09_18-15-01_meta.json`.
- Step 3b: `serve_metrics.py` now defaults to `--recordings-root data_scratchpad`;
  update the "reads recordings from the current directory" sentence
  ([how to use.md:22-23](../../../../Repos/ProtoSom/how%20to%20use.md#L22-L23)).
- Step 5: the `deploy.py --zarr/--meta` example gets the `data_scratchpad/` prefix.

---

## 2. Stop tracking the generated (PII-bearing) files

- `git rm --cached biometric_2026-07-08_01-57-52_meta.json
  biometric_2026-07-09_18-15-01_meta.json events.json` — untracked at HEAD, left on
  disk.
- Move the existing root artifacts into `data_scratchpad/`: the 5 `*.zarr` stores, the
  4 `biometric_*_meta.json` files, `events.json`. (`*.zarr/` is already gitignored;
  only the meta JSONs were leaking.)
- `.gitignore` additions — pattern-based, not exact-name, so a future timestamped run
  can never slip through again:

  ```
  # Pipeline outputs — generated per recording, may carry subject PII
  /data_scratchpad/
  *_meta.json
  !src_python/patient.example.json
  /events.json
  ```

  Note `*_meta.json` supersedes the existing exact-name lines
  `biometric_filtered_meta.json` and `biometric_meta.json` (lines 26-27) — collapse
  them into the new rule rather than leaving three overlapping entries.
- **`wiki/standards/privacy.md`**: rewrite the "Current state (good)" section. Its two
  factual errors are (a) "No real PII is, or ever was, committed" and (b) the claim
  that `.gitignore` excludes `*.edf` (it does not — `examples/**.edf` are tracked).
  Replace with an accurate statement: PII *was* committed in the three commits named
  above and remains in history; outputs are now gitignored by pattern. Add a rule that
  pipeline outputs must stay under `data_scratchpad/`.

---

## 3. Docker + build-cache root cleanup

- **`docker/` folder**: move `Dockerfile` → `docker/Dockerfile` and `docker_run.sh` →
  `docker/docker_run.sh`. `Dockerfile:37` is `COPY . .` and `:46` is
  `CMD [..., "/app/docker_run.sh"]` — the CMD path becomes
  `/app/docker/docker_run.sh`. Build context stays the repo root, so `.dockerignore`
  must remain at the root; document the new invocation
  (`docker build -f docker/Dockerfile .`) in the Dockerfile header comment and
  wherever the build command is written down.
  - Note in passing: `docker_run.sh` runs `read_log.py -f biometric_filtered.bin`, and
    no such file exists in the repo — the script is already stale. Out of scope to fix,
    but worth flagging while touching it.
- **Quartz build cache**: in [web/build.sh](../../../../Repos/ProtoSom/web/build.sh),
  change `QUARTZ_DIR="$ROOT/.quartz"` → `"$ROOT/build/quartz"` and the build output
  `-o "$ROOT/public"` → `-o "$ROOT/build/site"`, plus the three echoed paths and the
  header comment (which names `.quartz` and `public` several times).
  - `.gitignore` line 11 already has `build/`, so `/.quartz/` and `/public/` (lines
    21-22) can be deleted.
  - `.github/workflows/deploy-pages.yml`: cache `path: .quartz` → `build/quartz`
    (line 48) and `upload-pages-artifact` `path: public` → `build/site` (line 58).
- **Stray root `node_modules/`**: contains only `.vite/vitest`, a cache left by running
  vitest from the root instead of `src_web/`. Delete the directory and add
  `/node_modules/` to `.gitignore` (currently only `node_modules/.vite/` and
  `src_web/node_modules/` are covered).

### Deliberately left alone

`.husky/`, `.agents/`, `.github/`, `.vscode/`, `.claudeignore`, `.gitattributes`,
`licence.txt`, `README.md` — all root-required by convention or tooling.
`.husky/` is worth one note: no `core.hooksPath` is set and `.git/hooks` holds only
samples, so those hooks are **not currently active** — a separate issue, not part of
this cleanup.

`doc/`, `plans/`, `Deprecated/`, `examples/` and `how to use.md` stay where they are per
the scope decision. `examples/` is 43 MB of committed `.edf` recordings and is the
largest remaining candidate if repo size becomes a concern later.

---

## Verification

1. **Wiki gate** — this repo requires reading all `wiki/**/*.md` before code changes
   (16 files) and printing the count; do that first.
2. **End-to-end pipeline**, from the repo root:
   ```
   python .\src_python\read_log.py -f ".\examples\07 July 2026\biometric_2026-07-08_01-57-52.edf" -c --threshold 3 --skip 2 --ignore_last 2
   ```
   Confirm: nothing new appears in the repo root; `data_scratchpad/` contains
   `biometric_2026-07-08_01-57-52.zarr/`, `..._meta.json` and `events.json`; the
   printed "To view:" line carries the `data_scratchpad/` prefix.
3. **Viewer** — run the printed `serve.py --meta …` command, and also bare
   `python src_python/serve.py` (no args) to confirm auto-discovery finds the newest
   meta inside `data_scratchpad/`. Chart renders with data.
4. **Metrics service** — `python src_python/serve_metrics.py`, then drag-select a range
   in the chart; the Windowed PLMI card returns a number rather than "metrics service
   unavailable" (this proves the new default `--recordings-root` resolves).
5. **Deploy path (no network)** — `python mock_deploy_run.py`; it must still resolve
   zarr/meta and build the manifest.
6. **Python tests** — `python -m pytest src_python/tests`.
7. **Git hygiene** — `git status` shows a clean root (no `biometric_*`, no
   `events.json`, no `node_modules/`); `git check-ignore -v data_scratchpad/x_meta.json`
   reports the new rule; `git ls-files | grep -c nhs` style check confirms no tracked
   file carries PII (`git grep -l nhs_number` returns nothing).
8. **Docker** — `docker build -f docker/Dockerfile -t protosom .` succeeds.
9. **Wiki site** — `bash web/build.sh` produces `build/site/index.html` and creates
   nothing at the root.
