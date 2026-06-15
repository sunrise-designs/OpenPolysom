#ifndef HR_I2C_H
#define HR_I2C_H

#include <cstddef>
#include <cstdint>
#include <pthread.h>

// Reads 10-byte frames from the ESP32-S3 I2C slave at 100 Hz.
// Frame layout (big-endian): [ECG_H, ECG_L, X_H, X_L, Y_H, Y_L, Z_H, Z_L, RR_H, RR_L]
//   ECG     — raw 12-bit AD8232 sample
//   X/Y/Z   — raw 12-bit ADXL335 axis samples
//   RR      — latest RR interval in ms relayed from Polar H9 via ESP32 BLE
class HrI2c {
public:
    HrI2c(const char* i2c_dev = "/dev/i2c-1", uint8_t addr = 0x30);
    ~HrI2c();

    bool start();
    void stop();

    // Pops up to count ECG samples into buf (as double). Pads with 0.0 if ring is empty.
    void drain(double* buf, size_t count);

    // Returns the latest HR (BPM) and RR (ms) derived from the Polar H9 data.
    // Returns false if no valid RR has been received yet.
    bool getLatestHR(int& hr, int& rr) const;

    // Returns the latest raw ADXL335 axis readings (12-bit, 0–4095).
    // Returns false if no frame has been received yet.
    bool getLatestAccel(int& x, int& y, int& z) const;

private:
    static void* threadFunc(void* arg);

    const char* i2c_dev_;
    uint8_t addr_;
    int fd_;
    pthread_t thread_;
    mutable pthread_mutex_t lock_;
    bool running_;

    static const size_t RING_SIZE = 512;
    uint16_t ring_[RING_SIZE];
    size_t head_;
    size_t tail_;
    size_t avail_;

    int latestHr_;
    int latestRr_;
    bool hasHR_;

    int latestAccelX_;
    int latestAccelY_;
    int latestAccelZ_;
    bool hasAccel_;
};

#endif // HR_I2C_H
