#!/usr/bin/env python3
"""Send a binary time-sync command to the ESP32-C6 heart-idf firmware.

The firmware (components/time_sync) waits at boot for this frame on its
console UART (115200 baud — the same port used for flashing/monitoring)
whenever no DS1307 RTC is fitted or its stored time isn't valid yet.

Frame format (little-endian, no padding, 51 bytes total):

    offset  size  field
    0       2     magic:      0xA5, 0x5A
    2       8     unix_time_s: int64, seconds since Unix epoch, UTC
    10      40    tz:          POSIX TZ string, NUL-terminated, zero-padded
    50      1     checksum:    sum of bytes [0:50], truncated to uint8

Usage:
    python tools/set_time.py COM5
    python tools/set_time.py /dev/ttyACM0 --tz "GMT0BST,M3.5.0/1,M10.5.0"

Requires: pyserial (pip install pyserial)
"""
import argparse
import struct
import sys
import time

import serial

MAGIC = bytes([0xA5, 0x5A])
TZ_LEN = 40
DEFAULT_BAUD = 115200
DEFAULT_TZ = "GMT0BST,M3.5.0/1,M10.5.0"  # UK time, matches the firmware's own default


def build_frame(unix_time_s: int, tz: str) -> bytes:
    tz_bytes = tz.encode("ascii")
    if len(tz_bytes) >= TZ_LEN:
        raise ValueError(f"tz string too long ({len(tz_bytes)} bytes, max {TZ_LEN - 1})")
    tz_field = tz_bytes.ljust(TZ_LEN, b"\x00")

    body = MAGIC + struct.pack("<q", unix_time_s) + tz_field
    checksum = sum(body) & 0xFF
    return body + bytes([checksum])


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("port", help="Serial port, e.g. COM5 or /dev/ttyACM0")
    parser.add_argument(
        "--tz",
        default=DEFAULT_TZ,
        help=f"POSIX TZ string to set on the device (default: {DEFAULT_TZ!r}). "
        "Pass an empty string to leave the device's current TZ unchanged.",
    )
    parser.add_argument(
        "--epoch",
        type=int,
        default=None,
        help="Unix timestamp (UTC seconds) to send; defaults to the current time",
    )
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help=f"Baud rate (default {DEFAULT_BAUD})")
    args = parser.parse_args()

    unix_time_s = args.epoch if args.epoch is not None else int(time.time())
    frame = build_frame(unix_time_s, args.tz)

    with serial.Serial(args.port, args.baud, timeout=5) as ser:
        ser.write(frame)
        ser.flush()

    print(
        f"Sent time-sync frame: unix_time_s={unix_time_s} "
        f"({time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime(unix_time_s))}), "
        f"tz={args.tz or '(unchanged)'}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
