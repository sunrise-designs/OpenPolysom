import serial, sys, time, struct

PORT = '/dev/ttyACM0'   # Linux default. Mac: /dev/cu.usbmodem*, Windows: COM3 etc.
BAUD = 115200
OUT  = 'biometric.bin'

if len(sys.argv) > 1:
    PORT = sys.argv[1]

print(f"Opening {PORT} at {BAUD} baud...")
ser = serial.Serial(PORT, BAUD, timeout=10)
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
with open(OUT, 'wb') as f:
    f.write(data)
print(f"Saved {len(data)} bytes to {OUT}")

# Quick decode preview of first 5 records
print("\nFirst records (accel_x, accel_y, accel_z in 8-bit, rr_ms):")
for i in range(0, min(len(data) - 4, 25), 5):
    x, y, z = data[i], data[i+1], data[i+2]
    rr = struct.unpack_from('<H', data, i + 3)[0]
    print(f"  [{i//5:4d}]  x={x:3d} y={y:3d} z={z:3d}  rr={rr} ms")
