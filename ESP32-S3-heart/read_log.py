import argparse
import serial, time, struct
from pathlib import Path
import numpy as np
from scipy.ndimage import median_filter
import matplotlib.pyplot as plt
import mpld3
import plotly.graph_objects as go
from plotly.subplots import make_subplots

PORT = 'COM4'   # Linux default. Mac: /dev/cu.usbmodem*, Windows: COM3 etc.
BAUD = 115200
OUT  = 'biometric.bin'
HTML_OUT = 'biometric.html'
PLOTLY_HTML_OUT = 'biometric_plotly.html'


def dump_from_device(port, baud, out):
    print(f"Opening {port} at {baud} baud...")
    ser = serial.Serial(port, baud, timeout=10)
    time.sleep(2)  # wait for ESP32 to finish booting after port open resets it

    ser.reset_input_buffer()
    ser.write(b'D\n')

    # Wait for DUMP_START header
    expected = 0
    for _ in range(200):
        line = ser.readline().decode(errors='ignore').strip()
        if line.startswith('DUMP_START:'):
            expected = int(line.split(':')[1])
            print(f"Receiving {expected} bytes ({expected // 5} records)...")
            break

    hex_str = ''
    while True:
        line = ser.readline().decode(errors='ignore').strip()
        if line == 'DUMP_END':
            break
        hex_str += line

    ser.close()

    data = bytes.fromhex(hex_str)
    with open(out, 'wb') as f:
        f.write(data)
    print(f"Saved {len(data)} bytes to {out}")
    return data


def erase_device(port, baud):
    print(f"Opening {port} at {baud} baud...")
    ser = serial.Serial(port, baud, timeout=10)
    time.sleep(2)  # wait for ESP32 to finish booting after port open resets it

    ser.reset_input_buffer()
    ser.write(b'E\n')

    line = ser.readline().decode(errors='ignore').strip()
    print(line if line else "(no response)")

    ser.close()


def load_from_file(path):
    with open(path, 'rb') as f:
        data = f.read()
    print(f"Loaded {len(data)} bytes from {path}")
    return data


def save_plotly_html(t, rr, accel_x, accel_y, accel_z):
    fig = make_subplots(rows=4, cols=1, shared_xaxes=True,
                         subplot_titles=('RR (ms)', 'Accel X', 'Accel Y', 'Accel Z'))

    fig.add_trace(go.Scatter(x=t, y=rr, mode='lines', line=dict(color='red')), row=1, col=1)
    fig.add_trace(go.Scatter(x=t, y=accel_x, mode='lines', line=dict(color='blue')), row=2, col=1)
    fig.add_trace(go.Scatter(x=t, y=accel_y, mode='lines', line=dict(color='green')), row=3, col=1)
    fig.add_trace(go.Scatter(x=t, y=accel_z, mode='lines', line=dict(color='orange')), row=4, col=1)

    fig.update_xaxes(title_text='Time (s)', row=4, col=1)
    fig.update_layout(title='Biometric log', showlegend=False, height=800)

    fig.write_html(PLOTLY_HTML_OUT, include_plotlyjs=True)
    print(f"Saved interactive chart to {PLOTLY_HTML_OUT}")


def remove_baseline(data, window_sec=30, fs=10):
    records = [(data[i], data[i+1], data[i+2], struct.unpack_from('<H', data, i+3)[0])
               for i in range(0, len(data) - 4, 5)]

    channels = [np.array([r[c] for r in records], dtype=float) for c in range(3)]
    rr_vals  = [r[3] for r in records]

    window = int(window_sec * fs)
    # reflect mode avoids one-sided lag artifacts at the edges
    filtered = [ch - median_filter(ch, size=window, mode='reflect') + 128.0
                for ch in channels]

    out = bytearray()
    for i, rr in enumerate(rr_vals):
        ax = int(np.clip(round(filtered[0][i]), 0, 255))
        ay = int(np.clip(round(filtered[1][i]), 0, 255))
        az = int(np.clip(round(filtered[2][i]), 0, 255))
        out += bytes([ax, ay, az]) + struct.pack('<H', rr)
    return bytes(out)


def plot(data):
    records = [(data[i], data[i+1], data[i+2], struct.unpack_from('<H', data, i+3)[0])
               for i in range(0, len(data) - 4, 5)]

    accel_x = [r[0] for r in records]
    accel_y = [r[1] for r in records]
    accel_z = [r[2] for r in records]
    rr      = [r[3] for r in records]
    t       = [i / 10.0 for i in range(len(records))]  # seconds at 10 Hz

    fig, axes = plt.subplots(4, 1, figsize=(12, 8), sharex=True)
    fig.suptitle('Biometric log')

    axes[0].plot(t, rr, color='tab:red')
    axes[0].set_ylabel('RR (ms)')

    axes[1].plot(t, accel_x, color='tab:blue')
    axes[1].set_ylabel('Accel X')

    axes[2].plot(t, accel_y, color='tab:green')
    axes[2].set_ylabel('Accel Y')

    axes[3].plot(t, accel_z, color='tab:orange')
    axes[3].set_ylabel('Accel Z')
    axes[3].set_xlabel('Time (s)')

    plt.tight_layout()
    mpld3.save_html(fig, HTML_OUT)
    print(f"Saved interactive chart to {HTML_OUT}")
    save_plotly_html(t, rr, accel_x, accel_y, accel_z)
    plt.show()


def main():
    parser = argparse.ArgumentParser(description='Dump biometric log from ESP32 device and plot it.')
    parser.add_argument('port', nargs='?', default=PORT, help=f'Serial port (default: {PORT})')
    parser.add_argument('-f', '--file', help='Re-create the chart from a previously saved .bin file instead of reading from the device')
    parser.add_argument('-e', '--erase', action='store_true', help='Erase the log on the device and exit (sends the "E" command)')
    parser.add_argument('-b', '--baseline', action='store_true', help='Remove baseline from accelerometer channels and write a new <stem>_filtered.bin file, then exit')
    parser.add_argument('--window', type=float, default=30.0, help='Median filter window in seconds for baseline removal (default: 30)')
    args = parser.parse_args()

    if args.erase:
        erase_device(args.port, BAUD)
        return

    if args.file:
        data = load_from_file(args.file)
    else:
        data = dump_from_device(args.port, BAUD, OUT)

    if args.baseline:
        src = Path(args.file) if args.file else Path(OUT)
        out_path = src.with_stem(src.stem + '_filtered')
        filtered = remove_baseline(data, window_sec=args.window)
        out_path.write_bytes(filtered)
        print(f"Saved baseline-removed data to {out_path}")
        return

    plot(data)


if __name__ == '__main__':
    main()
