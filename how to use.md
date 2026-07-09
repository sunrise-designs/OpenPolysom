1. Run `src_python\install.bat` (or an equivalent for Linux)
2. Run command `python .\src_python\read_log.py -f ".\examples\07 July 2026\biometric_2026-07-08_01-57-52.edf" -c --threshold 3 --skip 2 --ignore_last 2`
(or use another .edf file of interest)
This generates the output .html, and start the web server and browser to show the chart

Notes on the arguments (see `src_python/read_log.py`):
- `-f` takes an EDF+ recording (`.edf`)
- `--skip` / `--ignore_last` are seconds, not sample counts (the EDF's channels run at different native rates — 50 Hz accel vs 1 Hz RR — so a sample count is ambiguous).
- `--threshold` is an accelerometer amplitude threshold in **mg** (physical units read from the EDF+)

3. Then to deploy to a netlify server, run

`python src_python/deploy.py`

 or deploy a specific data set with 

`python src_python/deploy.py --zarr <path to data>.zarr --meta <path to metadata>.json`

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