#include "sdp800.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

static const uint8_t kSdpStartMeasurementCommand[2] = {0x36, 0x15};
static const uint8_t kSdpStopMeasurementCommand[2] = {0x3F, 0xF9};

Sdp800::Sdp800(const char* device, uint8_t address)
    : _i2c_fd(-1), _address(address) {
    strncpy(_devicePath, device, sizeof(_devicePath) - 1);
    _devicePath[sizeof(_devicePath) - 1] = '\0';
}

Sdp800::~Sdp800() {
    if (_i2c_fd >= 0) {
        write(_i2c_fd, kSdpStopMeasurementCommand, sizeof(kSdpStopMeasurementCommand));
        close(_i2c_fd);
    }
}

bool Sdp800::begin(const char* i2c_dev) {
    return IIC_begin(i2c_dev);
}

bool Sdp800::IIC_begin(const char* i2c_dev) {
    _i2c_fd = open(i2c_dev, O_RDWR);
    if (_i2c_fd < 0) {
        fprintf(stderr, "Sdp800: failed to open %s: %s\n", i2c_dev, strerror(errno));
        return false;
    }

    strncpy(_devicePath, i2c_dev, sizeof(_devicePath) - 1);
    _devicePath[sizeof(_devicePath) - 1] = '\0';

    if (ioctl(_i2c_fd, I2C_SLAVE, _address) < 0) {
        fprintf(stderr, "Sdp800: ioctl I2C_SLAVE 0x%02x failed: %s\n", _address, strerror(errno));
        close(_i2c_fd);
        _i2c_fd = -1;
        return false;
    }

    return true;
}

void Sdp800::IIC_close() {
    if (_i2c_fd >= 0) {
        close(_i2c_fd);
        _i2c_fd = -1;
    }
}

void Sdp800::set_iic_addr(uint8_t address) {
    _address = address;
    if (_i2c_fd >= 0) {
        if (ioctl(_i2c_fd, I2C_SLAVE, _address) < 0) {
            fprintf(stderr, "Sdp800: ioctl I2C_SLAVE 0x%02x failed: %s\n", _address, strerror(errno));
        }
    }
}

bool Sdp800::checkCrc(const uint8_t* data) const {
    uint8_t crc = 0xFF;
    for (int i = 0; i < 2; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x31;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc == data[2];
}

bool Sdp800::readPressureMbar(double& pressure_mbar) {
    if (_i2c_fd < 0) {
        return false;
    }

    if (write(_i2c_fd, kSdpStartMeasurementCommand, sizeof(kSdpStartMeasurementCommand)) != sizeof(kSdpStartMeasurementCommand)) {
        fprintf(stderr, "Sdp800: failed to write start measurement command: %s\n", strerror(errno));
        return false;
    }

    usleep(20000); // 20 ms to allow the SDP800 to produce a sample at 50 Hz

    uint8_t buffer[9];
    ssize_t n = read(_i2c_fd, buffer, sizeof(buffer));
    if (n < 3) {
        return false;
    }

    if (!checkCrc(buffer)) {
        return false;
    }

    int16_t rawPressure = static_cast<int16_t>((buffer[0] << 8) | buffer[1]);
    double pressurePa = static_cast<double>(rawPressure) * 125.0 / 32768.0;
    pressure_mbar = pressurePa / 100.0;
    return true;
}
