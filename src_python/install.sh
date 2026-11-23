#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

which python3

python3 -m pip install --upgrade pip
python3 -m pip install -r "$SCRIPT_DIR/requirements.txt"

# ---------------------------------------------------------------------------
# Web viewer: needs Node.js to build src_web/dist/chart.js from TypeScript.
# ---------------------------------------------------------------------------
if ! command -v npm >/dev/null 2>&1; then
    echo
    echo "Node.js not found - trying to install it with the system package manager..."
    if command -v apt-get >/dev/null 2>&1; then
        sudo apt-get update && sudo apt-get install -y nodejs npm
    elif command -v dnf >/dev/null 2>&1; then
        sudo dnf install -y nodejs npm
    elif command -v pacman >/dev/null 2>&1; then
        sudo pacman -S --needed --noconfirm nodejs npm
    elif command -v brew >/dev/null 2>&1; then
        brew install node
    fi
fi

if ! command -v npm >/dev/null 2>&1; then
    echo
    echo "ERROR: Node.js/npm is not available, so the web viewer was not built." >&2
    echo "Install Node.js LTS (https://nodejs.org) and run this script again." >&2
    exit 1
fi

cd "$SCRIPT_DIR/../src_web"
npm install
npm run build

echo
echo "Install complete."
