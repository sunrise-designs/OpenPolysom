import asyncio
import csv
import json
import socket
import struct
from datetime import datetime
from bleak import BleakClient, BleakError
from config import POLAR_HR_SOCKET_PATH

ADDRESS = "CA:5A:0C:71:2A:94"
HR_MEASUREMENT_UUID = "00002a37-0000-1000-8000-00805f9b34fb"
HR_TIMEOUT_SECONDS = 10
RECONNECT_DELAY_SECONDS = 5
RECONNECT_LOG_FILE = "ble_reconnects.json"
UDS_SOCKET_PATH = POLAR_HR_SOCKET_PATH


def make_filename():
    return datetime.now().strftime("polar_hr_data_%Y%m%d_%H%M%S.csv")


class ReconnectLog:
    def __init__(self, path):
        self.path = path
        try:
            with open(path) as f:
                self.entries = json.load(f)
        except (FileNotFoundError, json.JSONDecodeError):
            self.entries = []

    def record(self, reason: str, attempt: int):
        entry = {
            "timestamp": datetime.now().isoformat(timespec="milliseconds"),
            "attempt": attempt,
            "reason": reason,
        }
        self.entries.append(entry)
        with open(self.path, "w") as f:
            json.dump(self.entries, f, indent=2)
        print(f"[reconnect #{attempt}] reason={reason!r} logged to {self.path}")


class HRLogger:
    def __init__(self, filename):
        self.filename = filename
        self.file = open(self.filename, mode="w", newline="")
        self.writer = csv.writer(self.file)
        self.writer.writerow(["Timestamp", "HR_BPM", "RRi_ms"])
        self.last_update = asyncio.get_event_loop().time()
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        print(f"Logging data to {self.filename}...")

    def _send_to_socket(self, hr, rr_ms):
        # Convert RR interval to an integer timestamp in milliseconds.
        # If no RR interval exists, send 0 as a placeholder.
        rr_value = int(round(rr_ms)) if rr_ms is not None else 0

        # Send a raw binary payload containing two unsigned 16-bit values:
        #   1. hr (BPM)
        #   2. latest RR interval in ms
        payload = struct.pack("<HH", hr, rr_value)
        try:
            self.socket.sendto(payload, UDS_SOCKET_PATH)
        except OSError as e:
            print(f"Warning: failed to send socket message: {e}")

    def notification_handler(self, sender, data):
        self.last_update = asyncio.get_event_loop().time()
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")

        # Flags are encoded in the first byte of the HR measurement characteristic.
        flags = data[0]
        hr_is_uint16 = flags & 0x01
        ee_present = (flags & 0x08) >> 3
        rr_present = (flags & 0x10) >> 4

        offset = 1
        if hr_is_uint16:
            # HR value is encoded as UINT16
            hr = int.from_bytes(data[offset:offset + 2], byteorder="little")
            offset += 2
        else:
            # HR value is encoded as UINT8
            hr = data[offset]
            offset += 1

        # Skip the optional Energy Expended field if present.
        if ee_present:
            offset += 2

        rr_intervals = []
        if rr_present:
            # There may be one or more RR interval values following the HR field.
            while offset + 1 < len(data):
                rr_raw = int.from_bytes(data[offset:offset + 2], byteorder="little")
                rr_ms = (rr_raw / 1024.0) * 1000.0
                rr_intervals.append(round(rr_ms, 2))
                offset += 2

        latest_rr = rr_intervals[-1] if rr_intervals else None
        self.writer.writerow([timestamp, hr, rr_intervals])
        self.file.flush()
        self._send_to_socket(hr, latest_rr)
        print(f"[{timestamp}] Heart Rate: {hr} BPM | RRi (ms): {rr_intervals}")

    def close(self):
        self.file.close()
        self.socket.close()


async def run_session(logger: HRLogger, reconnect_log: ReconnectLog, attempt: int):
    """Connect, stream HR, and return when disconnected or timed out."""
    print(f"Connecting to Polar H9 ({ADDRESS})... (attempt #{attempt})")
    async with BleakClient(ADDRESS, timeout=15.0) as client:
        if not client.is_connected:
            raise BleakError("Failed to connect")
        print("Connected! Recording... (Press Ctrl+C to stop)")
        logger.last_update = asyncio.get_event_loop().time()
        await client.start_notify(HR_MEASUREMENT_UUID, logger.notification_handler)

        while True:
            await asyncio.sleep(1)
            if not client.is_connected:
                raise BleakError("Connection dropped")
            elapsed = asyncio.get_event_loop().time() - logger.last_update
            if elapsed >= HR_TIMEOUT_SECONDS:
                raise TimeoutError(f"No HR update for {elapsed:.1f}s")


async def main():
    logger = HRLogger(make_filename())
    reconnect_log = ReconnectLog(RECONNECT_LOG_FILE)
    attempt = 0

    try:
        while True:
            attempt += 1
            try:
                await run_session(logger, reconnect_log, attempt)
            except KeyboardInterrupt:
                raise
            except TimeoutError as e:
                reason = str(e)
                print(f"Watchdog triggered: {reason}")
                reconnect_log.record(reason, attempt)
            except (BleakError, OSError) as e:
                reason = f"{type(e).__name__}: {e}"
                print(f"Connection error: {reason}")
                reconnect_log.record(reason, attempt)
            except Exception as e:
                reason = f"Unexpected {type(e).__name__}: {e}"
                print(f"Unexpected error: {reason}")
                reconnect_log.record(reason, attempt)

            print(f"Reconnecting in {RECONNECT_DELAY_SECONDS}s...")
            await asyncio.sleep(RECONNECT_DELAY_SECONDS)
    finally:
        logger.close()
        print(f"\nFile {logger.filename} saved and closed.")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
