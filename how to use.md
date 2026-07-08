1. Run `src_python\install.bat` (or an equivalent for Linux)
2. Run command `python .\src_python\read_log.py -f ".\examples\07 July 2026\biometric_2026-07-08_01-57-52.edf" -c --threshold 3 --skip 150 --ignore_last 250`
This generates the output .html, and start the web server and browser to show the chart

Notes on the arguments (see `src_python/read_log.py`):
- `-f` takes an EDF+ recording (`.edf`)
- `--skip` / `--ignore_last` are seconds, not sample counts (the EDF's channels run at different native rates — 50 Hz accel vs 1 Hz RR — so a sample count is ambiguous).
- `--threshold` is an accelerometer amplitude threshold in **mg** (physical units read from the EDF+)
