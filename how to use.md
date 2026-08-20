1. Run `src_python\install.bat` (or an equivalent for Linux)
2. Run command `python .\src_python\read_log.py -f ".\examples\07 July 2026\biometric_2026-07-08_01-57-52.edf" -c --threshold 3 --skip 2 --ignore_last 2`
(or use another .edf file of interest)
This generates the output .html, and start the web server and browser to show the chart

All generated output — the `.zarr` working store and its `_meta.json` / `events.json`
sidecars — is written to **`data_scratchpad/`**, not the repo root. That folder is
gitignored: `meta.json` can carry a `subject.pii` block, so nothing under it should ever
be committed. Override the location with `--out-dir` if you need to.

Notes on the arguments (see `src_python/read_log.py`):
- `-f` takes an EDF+ recording (`.edf`)
- `--skip` / `--ignore_last` are seconds, not sample counts (the EDF's channels run at different native rates — 50 Hz accel vs 1 Hz RR — so a sample count is ambiguous).
- `--threshold` is an accelerometer amplitude threshold in **mg** (physical units read from the EDF+)

3. If the read_log.py runs successfully, it will show the next suggested command, to render the results locally, for example:

`python src_python/serve.py --meta data_scratchpad/biometric_2026-07-09_18-15-01_meta.json`

With no arguments `serve.py` picks the newest `*_meta.json` in `data_scratchpad/`.

3b. (optional) To use the "Windowed PLMI" card — drag-select a range on a chart to
compute PLMI for just that window — also run the metrics service, in a separate
terminal, from the repo root:

`python src_python/serve_metrics.py`

This starts a small API on `http://localhost:8800` (the default `src_web/metrics-config.js`
points at) and reads recordings from `data_scratchpad/` (override with
`--recordings-root`). The chart works fine without it — you'll just see "metrics service
unavailable" in that card instead of a value if it isn't running.

4. (only if required) run npm build:
```
cd src_web
npm.cmd run build
```

5. Then to deploy to a netlify server, run

`python src_python/deploy.py`

 or deploy a specific data set with 

`python src_python/deploy.py --zarr data_scratchpad/<recording>.zarr --meta data_scratchpad/<recording>_meta.json`

For that to work, make sure you have a correct `netlify.json` in the scr_python folder which looks like:

```
{
  "site_id": "b2f98b73-xxxx-xxxx-xxxx-6f6d0a3f8xxxxx",
  "token": "xxx_t1gKKx3RjxxxxxxxxxxxxxUGsYPL7xxxxx"
}
```

Each deploy **adds** the study to the site rather than replacing it — the site's
root page lists every study that has been deployed so far, linking to each
one. The viewer (`index.html`) and chart bundle are shared across all studies
and only re-uploaded when their bytes actually change, so deploying a new
study is cheap regardless of how many studies are already live.