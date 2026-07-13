#ifndef SDP800_H
#define SDP800_H

#include <cstdint>

class Sdp800 {
public:
    explicit Sdp800(const char* device = "/dev/i2c-1", uint8_t address = 0x25);
    ~Sdp800();

    bool begin(const char* i2c_dev = "/dev/i2c-1");
    bool readPressureMbar(double& pressure_mbar);

    static constexpr int kSampleRateHz = 50;

private:
    bool checkCrc(const uint8_t* data) const;
    bool IIC_begin(const char* i2c_dev = "/dev/i2c-1");
    void IIC_close();
    void set_iic_addr(uint8_t address);

    int _i2c_fd;
    char _devicePath[64];
    uint8_t _address;
};

#endif // SDP800_H
