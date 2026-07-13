import argparse
import json
from pathlib import Path

from edf_reader import load_edf
from signal_processing import count_plm, compute_hrv, accel_magnitude, combine_bilateral_vm
from plotting import save_plotly_html, plot
from export_zarr import save_zarr_json


def load_recording_meta(edf_path):
    # Written alongside the .edf by the firmware (see logger.cpp write_json_sidecar);
    # falls back to the script directory for when they differ.
    candidates = [
        Path(edf_path).with_suffix('.json'),
        Path(__file__).parent / Path(edf_path).with_suffix('.json').name,
    ]
    for p in candidates:
        if p.exists():
            return json.loads(p.read_text())
    return None


def _trim(signal, fs, skip_s, ignore_last_s):
    start = int(skip_s * fs)
    end = len(signal) - int(ignore_last_s * fs) if ignore_last_s > 0 else len(signal)
    return signal[start:end]


def main():
    parser = argparse.ArgumentParser(description='Load a biometric EDF+ recording and plot it.')
    parser.add_argument('-f', '--file', required=True, help='Load from an EDF+ recording')
    parser.add_argument('--window', type=float, default=30.0, help='Median filter window in seconds for baseline removal (default: 30)')
    parser.add_argument('-c', '--count', action='store_true', help='Count PLMs per AASM scoring rules and print total count + PLMI')
    parser.add_argument('--threshold', type=float, default=8.0, help='Accelerometer amplitude threshold (mg) for LM detection (default: 8)')
    parser.add_argument('--skip', type=float, default=70.0, help='Seconds to trim from the start of each channel (default: 70)')
    parser.add_argument('--ignore_last', type=float, default=0.0, help='Seconds to trim from the end of each channel (default: 0)')
    parser.add_argument('--chart', choices=['echarts', 'plotly'], default='echarts',
                        help='HTML chart library for the output report (default: echarts)')
    parser.add_argument('--plotlyjs', choices=['cdn', 'embed', 'omit'], default='cdn',
                        help='How to include Plotly.js in the HTML: cdn (default), embed (self-contained), omit (fragment only)')
    args = parser.parse_args()

    _plotlyjs = {'cdn': 'cdn', 'embed': True, 'omit': False}[args.plotlyjs]

    src_path = Path(args.file)
    recording = load_edf(src_path)
    print(f"Loaded EDF+ recording starting {recording.start_datetime} from {src_path}")

    recording_meta = load_recording_meta(src_path)
    print(f"Recording metadata: {recording_meta}" if recording_meta else "No recording metadata found.")

    if args.skip > 0:
        print(f"Skipping first {args.skip:.1f} s of each channel")
    if args.ignore_last > 0:
        print(f"Ignoring last {args.ignore_last:.1f} s of each channel")

    def trimmed(label):
        return _trim(recording[label], recording.rate(label), args.skip, args.ignore_last)

    a0x, a0y, a0z = trimmed('Accel0X'), trimmed('Accel0Y'), trimmed('Accel0Z')
    a1x, a1y, a1z = trimmed('Accel1X'), trimmed('Accel1Y'), trimmed('Accel1Z')
    rr = trimmed('RR')
    fs_accel = recording.rate('Accel0X')
    fs_rr = recording.rate('RR')

    t = [i / fs_accel for i in range(len(a0x))]
    t_rr = [i / fs_rr for i in range(len(rr))]
    raw = ([float(v) for v in a0x], [float(v) for v in a0y], [float(v) for v in a0z])
    raw1 = ([float(v) for v in a1x], [float(v) for v in a1y], [float(v) for v in a1z])

    # Respiratory (Thoracic/Abdomen/Flow) — present on the RPi5 6-channel and
    # ESP32-C6 11-channel devices, absent on wrist-only logs.
    # No DSP yet (RIP baseline removal / airflow filtering is future work, see
    # wiki/knowledge/signal-processing.md); pass the physical-unit trace
    # straight through so the viewer can at least display it.
    respiratory = None
    if 'Thoracic' in recording.signals and 'Abdomen' in recording.signals and 'Flow' in recording.signals:
        respiratory = (
            [float(v) for v in trimmed('Thoracic')],
            [float(v) for v in trimmed('Abdomen')],
            [float(v) for v in trimmed('Flow')],
        )

    if args.count:
        print("--- Accel0 ---")
        result0 = count_plm(a0x, a0y, a0z, threshold=args.threshold, fs=fs_accel)
        print("--- Accel1 ---")
        result1 = count_plm(a1x, a1y, a1z, threshold=args.threshold, fs=fs_accel)

        print("--- Combined (bilateral) ---")
        combined = combine_bilateral_vm(result0['vm'], result1['vm'], threshold=args.threshold, fs=fs_accel)
        print(f"Recording duration : {combined['total_hours']:.2f} hours")
        print(f"LMs detected       : {combined['total_lms']}")
        print(f"PLMs (series ≥4, 5–90 s apart): {combined['total_plms']}")
        print(f"PLMI               : {combined['plmi']:.1f} /hour  [AASM threshold ≥15/hour for adults]")

        # The combined bilateral score (either leg moved) is the clinical
        # headline number; per-accelerometer results are carried alongside it
        # for the leg-independent chart panes and events.
        result = combined
        result.update({'threshold': args.threshold, 'window_sec': args.window, 'fs': fs_accel})

        hrv_overall, hrv_t, hrv_rmssd = compute_hrv(rr, fs=fs_rr)
        result.update({'hrv_overall': hrv_overall, 'hrv_t': hrv_t, 'hrv_rmssd': hrv_rmssd})
        print(f"HRV (RMSSD)        : {hrv_overall:.1f} ms")

        if args.chart == 'echarts':
            zarr_path, meta_path = save_zarr_json(
                src_path.stem,
                t, list(rr), raw, result['vm'],
                hrv_t=result['hrv_t'], hrv_rmssd=result['hrv_rmssd'],
                stats=result,
                recording_meta=recording_meta,
                source_path=src_path,
                skip_s=args.skip,
                rr_t=t_rr,
                accel1_mag=result1['vm'],
                leg_stats={'accel0': result0, 'accel1': result1},
                accel1_raw=raw1,
                respiratory_raw=respiratory,
            )
            print(f"To view: python src_python/serve.py --meta {meta_path}")
        else:
            save_plotly_html(
                t, list(rr), result['vm'],
                lm_events=result['lm_events'],
                plm_groups=result['plm_groups'],
                stats=result,
                recording_meta=recording_meta,
                raw_channels=raw,
                include_plotlyjs=_plotlyjs,
            )
        return

    mag = accel_magnitude(a0x, a0y, a0z, window_sec=args.window, fs=fs_accel)
    plot(t, list(rr), mag, recording_meta=recording_meta, raw_channels=raw, include_plotlyjs=_plotlyjs)


if __name__ == '__main__':
    main()
