#pragma once
#include <Wire.h>
#include <stdint.h>

class Sdp800 {
public:
    explicit Sdp800(uint8_t address = 0x25);
    ~Sdp800();

    // Starts continuous measurement; call once before readPressureMbar().
    bool begin(TwoWire *wire = &Wire);

    // Reads the latest measurement. Returns false on CRC error or I2C fault.
    bool readPressureMbar(float &pressure_mbar);

    static constexpr int kSampleRateHz = 50;

private:
    bool checkCrc(const uint8_t *data) const;

    TwoWire *_wire;
    uint8_t  _addr;
};
