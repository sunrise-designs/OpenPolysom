import argparse
import sys
from pathlib import Path

from export_zarr import serve_and_open

parser = argparse.ArgumentParser(description='Serve the biometric viewer in the browser.')
parser.add_argument('--stem', help='Base name shared by the .zarr dir and _meta.json (e.g. biometric_filtered)')
parser.add_argument('--meta', help='Path to a specific _meta.json file')
args = parser.parse_args()

if args.meta:
    meta_path = Path(args.meta)
elif args.stem:
    meta_path = Path(args.stem + '_meta.json')
else:
    candidates = sorted(Path.cwd().glob('*_meta.json'), key=lambda p: p.stat().st_mtime, reverse=True)
    if not candidates:
        print('Error: no *_meta.json found in cwd. Use --stem or --meta.', file=sys.stderr)
        sys.exit(1)
    meta_path = candidates[0]

if not meta_path.exists():
    print(f'Error: not found: {meta_path}', file=sys.stderr)
    sys.exit(1)

serve_and_open(meta_path.parent, meta_path.name)
