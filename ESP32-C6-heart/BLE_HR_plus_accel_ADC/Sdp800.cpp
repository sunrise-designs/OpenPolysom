#include "Sdp800.h"
#include <Arduino.h>

static const uint8_t kStartCmd[2] = {0x36, 0x15};  // start continuous measurement
static const uint8_t kStopCmd[2]  = {0x3F, 0xF9};  // stop measurement

Sdp800::Sdp800(uint8_t address) : _wire(nullptr), _addr(address) {}

Sdp800::~Sdp800() {
    if (!_wire) return;
    _wire->beginTransmission(_addr);
    _wire->write(kStopCmd, 2);
    _wire->endTransmission();
}

bool Sdp800::begin(TwoWire *wire) {
    _wire = wire;
    _wire->beginTransmission(_addr);
    _wire->write(kStartCmd, 2);
    if (_wire->endTransmission() != 0) return false;
    delay(10);  // wait for first sample (datasheet: 8 ms min)
    return true;
}

bool Sdp800::checkCrc(const uint8_t *data) const {
    uint8_t crc = 0xFF;
    for (int i = 0; i < 2; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    }
    return crc == data[2];
}

bool Sdp800::readPressureMbar(float &pressure_mbar) {
    if (!_wire) return false;

    if (_wire->requestFrom(_addr, (uint8_t)9) != 9) return false;

    uint8_t buf[9];
    for (int i = 0; i < 9; i++) buf[i] = _wire->read();

    if (!checkCrc(buf)) return false;

    int16_t raw = (int16_t)((buf[0] << 8) | buf[1]);
    float pressurePa = raw * 125.0f / 32768.0f;
    pressure_mbar = pressurePa / 100.0f;
    return true;
}
