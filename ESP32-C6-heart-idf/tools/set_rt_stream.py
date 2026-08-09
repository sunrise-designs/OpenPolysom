#!/usr/bin/env python3
"""Enable or disable real-time sample streaming on the ESP32-C6 firmware.

Streaming (components/rt_stream) serves the samples the logger writes to the
EDF+ over a Wi-Fi WebSocket, so the signals can be watched live. It is OFF by
default because the radio costs the power this board was built to save, and
because an unattended night recording has nobody watching it. Turn it on for a
supervised session; turn it off again afterwards.

Wireless rather than over this USB link, deliberately: the AD8232 puts
electrodes on the patient, and a USB tether to a mains-powered host is a
leakage-current path. Streaming happens on battery, over the air.

The setting is stored in the device's NVS and **takes effect on the next boot** —
bringing Wi-Fi up mid-recording would put radio TX current in the path of the SD
writes. Reset the board after running this.

Frame format (little-endian, no padding, 4 bytes total), read by the same serial
command server as tools/set_time.py:

    offset  size  field
    0       2     magic:    0xA5, 0x5B   (0x5A is the time-sync command)
    2       1     enabled:  0 = off, 1 = on
    3       1     checksum: sum of bytes [0:3], truncated to uint8

The user LED flashes five times when the device accepts the command.

Usage:
    python tools/set_rt_stream.py COM7 --on
    python tools/set_rt_stream.py /dev/ttyACM0 --off

After enabling and resetting, join the device's Wi-Fi access point
("ProtoSom-XXXXXX", the password is shown on the OLED) and open the viewer at:

    index.html?rt=192.168.4.1

Requires: pyserial (pip install pyserial)
"""
import argparse
import sys

import serial

MAGIC = bytes([0xA5, 0x5B])
DEFAULT_BAUD = 115200


def build_frame(enabled: bool) -> bytes:
    body = MAGIC + bytes([1 if enabled else 0])
    return body + bytes([sum(body) & 0xFF])


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("port", help="Serial port, e.g. COM7 or /dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--on", action="store_true", help="enable streaming")
    group.add_argument("--off", action="store_true", help="disable streaming")
    args = parser.parse_args()

    enabled = bool(args.on)
    frame = build_frame(enabled)

    try:
        with serial.Serial(args.port, args.baud, timeout=2) as ser:
            ser.write(frame)
            ser.flush()
    except serial.SerialException as exc:
        print(f"Failed to open {args.port}: {exc}", file=sys.stderr)
        return 1

    print(f"Sent: real-time streaming {'ENABLED' if enabled else 'DISABLED'} ({frame.hex()})")
    print("Watch for five LED flashes, then reset the board for it to take effect.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
