#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

which python3

python3 -m pip install --upgrade pip
python3 -m pip install -r "$SCRIPT_DIR/requirements.txt"
