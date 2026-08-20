#!/bin/bash
set -euo pipefail

# Default container entrypoint (see docker/Dockerfile's CMD). Runs the analysis
# pipeline over a committed example recording so `docker run` exercises the whole
# path end to end without needing a recording mounted in.
#
# Environment variables
# ---------------------
#   RECORDING  Path to the EDF+ recording to analyse, relative to the repo root
#              (or absolute). Must be an EDF+ file — `read_log.py -f` does not read
#              the retired .bin format. Anything under examples/ is baked into the
#              image by the Dockerfile's `COPY . .`; anything else has to be
#              bind-mounted in.
#              Default: examples/07 July 2026/biometric_2026-07-08_01-57-52.edf
#
#   OUT_DIR    Directory for the generated .zarr working store and its
#              _meta.json / events.json sidecars, passed through as read_log.py's
#              --out-dir. Resolved relative to the cwd (/app in the image). It is
#              gitignored on the host and lives inside the container's writable
#              layer, so bind-mount it to keep results after the container exits.
#              Default: data_scratchpad
#
# Examples:
#   docker run protosom
#   docker run -e RECORDING="examples/17 July 2026/biometric_2026-07-16_23-00-00.edf" protosom
#   docker run -v "$PWD/data_scratchpad:/app/data_scratchpad" protosom
#   docker run -e OUT_DIR=/out -v "$PWD/results:/out" protosom
#
# Run from the image's WORKDIR (/app), i.e. the repo root — both paths above are
# resolved relative to the cwd.
RECORDING="${RECORDING:-examples/07 July 2026/biometric_2026-07-08_01-57-52.edf}"
OUT_DIR="${OUT_DIR:-data_scratchpad}"

echo "============================================"
echo "  Step 1: read_log.py (-c analysis)"
echo "============================================"
echo "  input:  $RECORDING"
echo "  output: $OUT_DIR/"
echo ""

# Thresholds match the worked example in "how to use.md". --skip/--ignore_last are
# SECONDS, not sample counts, so they must stay small relative to the recording
# length (these examples are ~1-2 h).
python src_python/read_log.py \
    -f "$RECORDING" \
    -c \
    --threshold 3 \
    --skip 2 \
    --ignore_last 2 \
    --out-dir "$OUT_DIR" \
    || {
        code=$?
        [ "$code" -eq 130 ] || { echo "read_log.py failed (exit $code)"; exit "$code"; }
    }

echo ""
echo "Results written to $OUT_DIR/ (bind-mount it to keep them after the container exits)."
ls -1 "$OUT_DIR"
