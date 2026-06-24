import argparse
import json
import struct
import time
from pathlib import Path

from device import dump_from_device, erase_device
from signal_processing import remove_baseline, count_plm, compute_hrv, accel_magnitude
from plotting import save_plotly_html, plot
from export_zarr import save_zarr_json

PORT = 'COM4'
BAUD = 115200
OUT  = f'biometric_{time.strftime("%Y-%m-%d")}.bin'


def load_from_file(path):
    with open(path, 'rb') as f:
        data = f.read()
    print(f"Loaded {len(data)} bytes from {path}")
    return data


def load_recording_meta(bin_path):
    # FUTURE: replace this function body with binary header parsing once
    # the firmware writes metadata into the .bin file header.
    # Until then, metadata lives in a sidecar .json file alongside the .bin,
    # with the script directory as a fallback for when they differ.
    candidates = [
        Path(bin_path).with_suffix('.json'),
        Path(__file__).parent / Path(bin_path).with_suffix('.json').name,
    ]
    for p in candidates:
        if p.exists():
            return json.loads(p.read_text())
    return None


def main():
    parser = argparse.ArgumentParser(description='Dump biometric log from ESP32 device and plot it.')
    parser.add_argument('port', nargs='?', default=PORT, help=f'Serial port (default: {PORT})')
    parser.add_argument('-f', '--file', help='Load from a previously saved .bin file instead of reading from the device')
    parser.add_argument('-e', '--erase', action='store_true', help='Erase the log on the device and exit (sends the "E" command)')
    parser.add_argument('-b', '--baseline', action='store_true', help='Remove baseline from accelerometer channels and write a new <stem>_filtered.bin file, then exit')
    parser.add_argument('--window', type=float, default=30.0, help='Median filter window in seconds for baseline removal (default: 30)')
    parser.add_argument('-c', '--count', action='store_true', help='Count PLMs per AASM scoring rules and print total count + PLMI')
    parser.add_argument('--threshold', type=float, default=8.0, help='Accelerometer amplitude threshold for LM detection (default: 8)')
    parser.add_argument('--skip', type=int, default=700, help='Number of leading samples to ignore (default: 700, file is not modified)')
    parser.add_argument('--ignore_last', type=int, default=0, help='Number of trailing samples to ignore (default: 0, file is not modified)')
    parser.add_argument('--chart', choices=['echarts', 'plotly'], default='echarts',
                        help='HTML chart library for the output report (default: echarts)')
    parser.add_argument('--plotlyjs', choices=['cdn', 'embed', 'omit'], default='cdn',
                        help='How to include Plotly.js in the HTML: cdn (default), embed (self-contained), omit (fragment only)')
    args = parser.parse_args()

    _plotlyjs = {'cdn': 'cdn', 'embed': True, 'omit': False}[args.plotlyjs]

    if args.erase:
        erase_device(args.port, BAUD)
        return

    if args.file:
        data = load_from_file(args.file)
        src_path = Path(args.file)
    else:
        data = dump_from_device(args.port, BAUD, OUT)
        src_path = Path(OUT)

    recording_meta = load_recording_meta(src_path)
    print(f"Recording metadata: {recording_meta}" if recording_meta else "No recording metadata found.")

    if args.skip > 0:
        data = data[args.skip * 5:]
        print(f"Skipping first {args.skip} samples ({args.skip / 10.0:.1f} s)")

    if args.ignore_last > 0:
        data = data[:-args.ignore_last * 5]
        print(f"Ignoring last {args.ignore_last} samples ({args.ignore_last / 10.0:.1f} s)")

    if args.baseline:
        src      = Path(args.file) if args.file else Path(OUT)
        out_path = src.with_stem(src.stem + '_filtered')
        out_path.write_bytes(remove_baseline(data, window_sec=args.window))
        print(f"Saved baseline-removed data to {out_path}")
        return

    if args.count:
        result = count_plm(data, threshold=args.threshold)
        result.update({'threshold': args.threshold, 'window_sec': args.window, 'fs': 10})
        records = [(data[i], data[i+1], data[i+2], struct.unpack_from('<H', data, i+3)[0])
                   for i in range(0, len(data) - 4, 5)]
        t       = [i / 10.0 for i in range(len(records))]
        rr_list = [r[3] for r in records]
        hrv_overall, hrv_t, hrv_rmssd = compute_hrv(rr_list)
        result.update({'hrv_overall': hrv_overall, 'hrv_t': hrv_t, 'hrv_rmssd': hrv_rmssd})
        print(f"HRV (RMSSD)        : {hrv_overall:.1f} ms")
        raw = ([r[0] for r in records], [r[1] for r in records], [r[2] for r in records])
        if args.chart == 'echarts':
            zarr_path, meta_path = save_zarr_json(
                src_path.stem,
                t, rr_list, raw, result['vm'],
                hrv_t=result['hrv_t'], hrv_rmssd=result['hrv_rmssd'],
                stats=result,
                recording_meta=recording_meta,
            )
            print(f"To view: python src_python/serve.py --meta {meta_path}")
        else:
            save_plotly_html(
                t, rr_list, result['vm'],
                lm_events=result['lm_events'],
                plm_groups=result['plm_groups'],
                stats=result,
                recording_meta=recording_meta,
                raw_channels=raw,
                include_plotlyjs=_plotlyjs,
            )
        return

    records = [(data[i], data[i+1], data[i+2], struct.unpack_from('<H', data, i+3)[0])
               for i in range(0, len(data) - 4, 5)]
    t   = [i / 10.0 for i in range(len(records))]
    rr  = [r[3] for r in records]
    mag = accel_magnitude(data, window_sec=args.window)
    raw = ([r[0] for r in records], [r[1] for r in records], [r[2] for r in records])
    plot(t, rr, mag, recording_meta=recording_meta, raw_channels=raw, include_plotlyjs=_plotlyjs)


if __name__ == '__main__':
    main()
