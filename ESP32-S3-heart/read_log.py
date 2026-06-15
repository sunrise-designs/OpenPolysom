import serial, sys, time, struct
import matplotlib.pyplot as plt

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
plt.show()
