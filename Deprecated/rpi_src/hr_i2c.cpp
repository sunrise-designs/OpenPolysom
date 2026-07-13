#include "hr_i2c.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

static const int SAMPLE_INTERVAL_US = 10000; // 100 Hz

HrI2c::HrI2c(const char* i2c_dev, uint8_t addr)
    : i2c_dev_(i2c_dev), addr_(addr), fd_(-1), running_(false),
      head_(0), tail_(0), avail_(0),
      latestHr_(0), latestRr_(0), hasHR_(false),
      latestAccelX_(0), latestAccelY_(0), latestAccelZ_(0), hasAccel_(false) {
    pthread_mutex_init(&lock_, nullptr);
}

HrI2c::~HrI2c() {
    stop();
    pthread_mutex_destroy(&lock_);
}

bool HrI2c::start() {
    if (running_) return true;

    fd_ = open(i2c_dev_, O_RDWR);
    if (fd_ < 0) {
        fprintf(stderr, "HrI2c: open(%s) failed: %s\n", i2c_dev_, strerror(errno));
        return false;
    }
    if (ioctl(fd_, I2C_SLAVE, addr_) < 0) {
        fprintf(stderr, "HrI2c: ioctl I2C_SLAVE 0x%02x failed: %s\n", addr_, strerror(errno));
        close(fd_);
        fd_ = -1;
        return false;
    }

    running_ = true;
    if (pthread_create(&thread_, nullptr, threadFunc, this) != 0) {
        fprintf(stderr, "HrI2c: pthread_create failed\n");
        running_ = false;
        close(fd_);
        fd_ = -1;
        return false;
    }
    return true;
}

void HrI2c::stop() {
    if (!running_) return;
    running_ = false;
    pthread_join(thread_, nullptr);
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

void HrI2c::drain(double* buf, size_t count) {
    pthread_mutex_lock(&lock_);
    for (size_t i = 0; i < count; i++) {
        if (avail_ > 0) {
            buf[i] = static_cast<double>(ring_[tail_]);
            tail_ = (tail_ + 1) % RING_SIZE;
            avail_--;
        } else {
            buf[i] = 0.0;
        }
    }
    pthread_mutex_unlock(&lock_);
}

bool HrI2c::getLatestAccel(int& x, int& y, int& z) const {
    pthread_mutex_lock(&lock_);
    bool ok = hasAccel_;
    if (ok) {
        x = latestAccelX_;
        y = latestAccelY_;
        z = latestAccelZ_;
    }
    pthread_mutex_unlock(&lock_);
    return ok;
}

bool HrI2c::getLatestHR(int& hr, int& rr) const {
    pthread_mutex_lock(&lock_);
    bool ok = hasHR_;
    if (ok) {
        hr = latestHr_;
        rr = latestRr_;
    }
    pthread_mutex_unlock(&lock_);
    return ok;
}

void* HrI2c::threadFunc(void* arg) {
    HrI2c* self = static_cast<HrI2c*>(arg);
    uint16_t lastRr = 0;

    while (self->running_) {
        // ESP32-S3 slave sends 10 bytes big-endian: [ECG_H, ECG_L, X_H, X_L, Y_H, Y_L, Z_H, Z_L, RR_H, RR_L]
        uint8_t buf[10] = {};
        ssize_t n = read(self->fd_, buf, 10);

        uint16_t ecg = 0, ax = 0, ay = 0, az = 0, rr = 0;
        printf("Read %d bytes from I2C\n", n);
        if (n == 10) {
            ecg = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
            ax  = (static_cast<uint16_t>(buf[2]) << 8) | buf[3];
            ay  = (static_cast<uint16_t>(buf[4]) << 8) | buf[5];
            az  = (static_cast<uint16_t>(buf[6]) << 8) | buf[7];
            rr  = (static_cast<uint16_t>(buf[8]) << 8) | buf[9];
        }

        pthread_mutex_lock(&self->lock_);

        self->ring_[self->head_] = ecg;
        self->head_ = (self->head_ + 1) % RING_SIZE;
        if (self->avail_ < RING_SIZE) {
            self->avail_++;
        } else {
            self->tail_ = (self->tail_ + 1) % RING_SIZE;
        }

        self->latestAccelX_ = static_cast<int>(ax);
        self->latestAccelY_ = static_cast<int>(ay);
        self->latestAccelZ_ = static_cast<int>(az);
        self->hasAccel_ = (n == 10);

        if (rr > 0 && rr != lastRr) {
            lastRr = rr;
            self->latestRr_ = static_cast<int>(rr);
            self->latestHr_ = static_cast<int>(std::round(60000.0 / rr));
            self->hasHR_ = true;
        }

        pthread_mutex_unlock(&self->lock_);

        usleep(SAMPLE_INTERVAL_US);
    }
    return nullptr;
}
