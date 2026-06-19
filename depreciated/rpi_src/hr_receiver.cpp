#include "hr_receiver.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

HrReceiver::HrReceiver(const char* udsPath)
    : udsPath_(udsPath), udsSockFd_(-1), running_(false), latestHr_(0), latestRr_(0), hasValue_(false) {
    pthread_mutex_init(&lock_, nullptr);
}

HrReceiver::~HrReceiver() {
    stop();
    pthread_mutex_destroy(&lock_);
}

bool HrReceiver::openSocket() {
    udsSockFd_ = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (udsSockFd_ < 0) {
        fprintf(stderr, "HrReceiver: socket() failed: %s\n", strerror(errno));
        return false;
    }

    unlink(udsPath_);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, udsPath_, sizeof(addr.sun_path) - 1);

    if (bind(udsSockFd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        fprintf(stderr, "HrReceiver: bind(%s) failed: %s\n", udsPath_, strerror(errno));
        closeSocket();
        return false;
    }

    int flags = fcntl(udsSockFd_, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(udsSockFd_, F_SETFL, flags | O_NONBLOCK);
    }

    return true;
}

void HrReceiver::closeSocket() {
    if (udsSockFd_ >= 0) {
        close(udsSockFd_);
        udsSockFd_ = -1;
    }
    unlink(udsPath_);
}

bool HrReceiver::start() {
    if (running_) {
        return true;
    }

    if (!openSocket()) {
        return false;
    }

    running_ = true;
    if (pthread_create(&thread_, nullptr, readerThread, this) != 0) {
        fprintf(stderr, "HrReceiver: pthread_create failed\n");
        running_ = false;
        closeSocket();
        return false;
    }

    return true;
}

void HrReceiver::stop() {
    if (!running_) {
        return;
    }

    running_ = false;
    closeSocket();
    pthread_join(thread_, nullptr);
}

bool HrReceiver::getLatest(int& hr, int& rr) const {
    pthread_mutex_lock(&lock_);
    bool ok = hasValue_;
    if (ok) {
        hr = latestHr_;
        rr = latestRr_;
    }
    pthread_mutex_unlock(&lock_);
    return ok;
}

void* HrReceiver::readerThread(void* arg) {
    HrReceiver* self = static_cast<HrReceiver*>(arg);
    while (self->running_) {
        uint8_t buffer[4];
        ssize_t n = recv(self->udsSockFd_, buffer, sizeof(buffer), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                usleep(100000);
                continue;
            }
            fprintf(stderr, "HrReceiver: recv error: %s\n", strerror(errno));
            break;
        }
        if (n < 4) {
            usleep(100000);
            continue;
        }

        int hr = static_cast<int>(buffer[0] | (buffer[1] << 8));
        int rr = static_cast<int>(buffer[2] | (buffer[3] << 8));

        pthread_mutex_lock(&self->lock_);
        self->latestHr_ = hr;
        self->latestRr_ = rr;
        self->hasValue_ = true;
        pthread_mutex_unlock(&self->lock_);
    }

    return nullptr;
}
