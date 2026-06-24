#!/bin/bash
set -euo pipefail

echo "============================================"
echo "  Step 1: read_log.py (-c analysis)"
echo "============================================"
echo ""

python src_python/read_log.py \
    -f biometric_filtered.bin \
    -c \
    --threshold 3 \
    --skip 1500 \
    --ignore_last 2500 \
    || {
        code=$?
        [ "$code" -eq 130 ] || { echo "read_log.py failed (exit $code)"; exit "$code"; }
    }
