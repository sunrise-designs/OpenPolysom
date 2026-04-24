#include "hr_i2c.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

static const int SAMPLE_INTERVAL_US = 10000; // 100 Hz

HrI2c::HrI2c(const char* i2c_dev, uint8_t addr)
    : i2c_dev_(i2c_dev), addr_(addr), fd_(-1), running_(false),
      head_(0), tail_(0), avail_(0) {
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

void* HrI2c::threadFunc(void* arg) {
    HrI2c* self = static_cast<HrI2c*>(arg);

    while (self->running_) {
        uint8_t buf[2] = {0, 0};
        ssize_t n = read(self->fd_, buf, 2);

        uint16_t value = 0;
        if (n == 2) {
            value = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
        }

        pthread_mutex_lock(&self->lock_);
        self->ring_[self->head_] = value;
        self->head_ = (self->head_ + 1) % RING_SIZE;
        if (self->avail_ < RING_SIZE) {
            self->avail_++;
        } else {
            // Ring full: discard oldest
            self->tail_ = (self->tail_ + 1) % RING_SIZE;
        }
        pthread_mutex_unlock(&self->lock_);

        usleep(SAMPLE_INTERVAL_US);
    }
    return nullptr;
}
