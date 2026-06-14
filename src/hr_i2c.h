#ifndef HR_I2C_H
#define HR_I2C_H

#include <cstddef>
#include <cstdint>
#include <pthread.h>

// Reads 6-byte frames from the ESP32-S3 I2C slave at 100 Hz.
// Frame layout (big-endian): [ECG_H, ECG_L, MAG_H, MAG_L, RR_H, RR_L]
//   ECG  — raw 12-bit AD8232 sample
//   MAG  — raw ADXL335 acceleration magnitude
//   RR   — latest RR interval in ms relayed from Polar H9 via ESP32 BLE
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
};

#endif // HR_I2C_H
