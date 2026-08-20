import argparse
import os

import uvicorn

from export_zarr import DEFAULT_OUT_DIR

parser = argparse.ArgumentParser(description='Serve the windowed clinical-metrics API.')
parser.add_argument('--host', default='127.0.0.1')
parser.add_argument('--port', type=int, default=8800)
parser.add_argument('--recordings-root', default=str(DEFAULT_OUT_DIR),
                     help='Directory containing <recording_id>.zarr + <recording_id>_meta.json '
                          f'(default: {DEFAULT_OUT_DIR}, where read_log.py writes them)')
args = parser.parse_args()

os.environ['PROTOSOM_RECORDINGS_ROOT'] = args.recordings_root

import metrics_service  # noqa: E402 — must import after setting the env var above

metrics_service.RECORDINGS_ROOT = metrics_service.Path(args.recordings_root)

uvicorn.run(metrics_service.app, host=args.host, port=args.port)
