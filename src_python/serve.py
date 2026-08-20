import argparse
import sys

from export_zarr import DEFAULT_OUT_DIR, find_meta, serve_and_open

parser = argparse.ArgumentParser(description='Serve the biometric viewer in the browser.')
parser.add_argument('--stem', help='Base name shared by the .zarr dir and _meta.json (e.g. biometric_filtered)')
parser.add_argument('--meta', help='Path to a specific _meta.json file')
args = parser.parse_args()

meta_path = find_meta(explicit=args.meta, stem=args.stem)
if meta_path is None:
    print(f'Error: no *_meta.json found in {DEFAULT_OUT_DIR}/ or cwd. Use --stem or --meta.',
          file=sys.stderr)
    sys.exit(1)

if not meta_path.exists():
    print(f'Error: not found: {meta_path}', file=sys.stderr)
    sys.exit(1)

serve_and_open(meta_path.parent, meta_path.name)
