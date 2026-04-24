#ifndef HR_I2C_H
#define HR_I2C_H

#include <cstddef>
#include <cstdint>
#include <pthread.h>

// Reads raw 12-bit ADC samples from an AD8232 (ESP32-S3 slave) via I2C at 100 Hz.
// The slave sends 2 bytes big-endian per master read request.
class HrI2c {
public:
    HrI2c(const char* i2c_dev = "/dev/i2c-1", uint8_t addr = 0x30);
    ~HrI2c();

    bool start();
    void stop();

    // Pops up to count samples into buf (as double). Pads with 0.0 if ring is empty.
    void drain(double* buf, size_t count);

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
};

#endif // HR_I2C_H
