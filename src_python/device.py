import serial
import time


def dump_from_device(port, baud, out):
    print(f"Opening {port} at {baud} baud...")
    ser = serial.Serial(port, baud, timeout=10)
    time.sleep(2)  # wait for ESP32 to finish booting after port open resets it

    ser.reset_input_buffer()
    ser.write(b'D\n')

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
