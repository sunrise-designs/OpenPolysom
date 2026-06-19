/*
    Seeed_LDC1612.cpp
    Driver for LDC1612 Inductive Sensor (Linux i2c-dev port)

    Copyright (c) 2018 Seeed Technology Co., Ltd.
    Website    : www.seeed.cc
    Author     : downey
    Create Time: June 2018
    Change Log : Ported to Linux i2c-dev for Raspberry Pi

    The MIT License (MIT)

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
    THE SOFTWARE.
*/

#include "Seeed_LDC1612.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

struct channel_params {
    float resistance;
    float inductance;
    float capacitance;
    float Q_factor;
    float Fsensor;
    u16 FREF_DIV;
    u16 FIN_DIV;
} channel_params[CHANNEL_NUM];


/**********************************************************************************************************/
/************************************************IIC PART************************************************/
/**********************************************************************************************************/

LDC1612_IIC_OPRTS::~LDC1612_IIC_OPRTS() {
    IIC_close();
}

bool LDC1612_IIC_OPRTS::IIC_begin(const char* i2c_dev) {
    _i2c_fd = open(i2c_dev, O_RDWR);
    if (_i2c_fd < 0) {
        fprintf(stderr, "IIC_begin: failed to open %s: %s\n", i2c_dev, strerror(errno));
        return false;
    }
    if (ioctl(_i2c_fd, I2C_SLAVE, _IIC_ADDR) < 0) {
        fprintf(stderr, "IIC_begin: ioctl I2C_SLAVE 0x%02x failed: %s\n", _IIC_ADDR, strerror(errno));
        close(_i2c_fd);
        _i2c_fd = -1;
        return false;
    }
    return true;
}

void LDC1612_IIC_OPRTS::IIC_close() {
    if (_i2c_fd >= 0) {
        close(_i2c_fd);
        _i2c_fd = -1;
    }
}

void LDC1612_IIC_OPRTS::set_iic_addr(u8 IIC_ADDR) {
    _IIC_ADDR = IIC_ADDR;
    if (_i2c_fd >= 0) {
        if (ioctl(_i2c_fd, I2C_SLAVE, _IIC_ADDR) < 0) {
            fprintf(stderr, "set_iic_addr: ioctl I2C_SLAVE 0x%02x failed: %s\n", _IIC_ADDR, strerror(errno));
        }
    }
}

s32 LDC1612_IIC_OPRTS::IIC_write_byte(u8 reg, u8 byte) {
    u8 buf[2] = {reg, byte};
    if (write(_i2c_fd, buf, 2) != 2) {
        fprintf(stderr, "IIC_write_byte: write failed: %s\n", strerror(errno));
        return ERROR_COMM;
    }
    return NO_ERROR;
}

s32 LDC1612_IIC_OPRTS::IIC_write_16bit(u8 reg, u16 value) {
    u8 buf[3] = {reg, (u8)(value >> 8), (u8)(value & 0xFF)};
    if (write(_i2c_fd, buf, 3) != 3) {
        fprintf(stderr, "IIC_write_16bit: write failed: %s\n", strerror(errno));
        return ERROR_COMM;
    }
    return NO_ERROR;
}

void LDC1612_IIC_OPRTS::IIC_read_byte(u8 reg, u8* byte) {
    if (write(_i2c_fd, &reg, 1) != 1) {
        fprintf(stderr, "IIC_read_byte: write reg failed: %s\n", strerror(errno));
        return;
    }
    if (read(_i2c_fd, byte, 1) != 1) {
        fprintf(stderr, "IIC_read_byte: read failed: %s\n", strerror(errno));
    }
}

void LDC1612_IIC_OPRTS::IIC_read_16bit(u8 start_reg, u16* value) {
    u8 buf[2] = {0, 0};
    if (write(_i2c_fd, &start_reg, 1) != 1) {
        fprintf(stderr, "IIC_read_16bit: write reg failed: %s\n", strerror(errno));
        return;
    }
    if (read(_i2c_fd, buf, 2) != 2) {
        fprintf(stderr, "IIC_read_16bit: read failed: %s\n", strerror(errno));
        return;
    }
    *value = ((u16)buf[0] << 8) | buf[1];
}


/**********************************************************************************************************/
/************************************************LDC1612 PART*********************************************/
/**********************************************************************************************************/

LDC1612::LDC1612(u8 IIC_ADDR) {
    set_iic_addr(IIC_ADDR);
}

s32 LDC1612::init(const char* i2c_dev) {
    if (!IIC_begin(i2c_dev)) {
        return ERROR_COMM;
    }
    return NO_ERROR;
}

void LDC1612::read_sensor_infomation() {
    u16 value = 0;
    IIC_read_16bit(READ_MANUFACTURER_ID, &value);
    printf("manufacturer id = 0x%04X\n", value);
    IIC_read_16bit(READ_DEVICE_ID, &value);
    printf("device id       = 0x%04X\n", value);
}

s32 LDC1612::single_channel_config(u8 channel) {
    /* 20 TURNS */
    set_Rp(CHANNEL_0, 15.727f);
    set_L(CHANNEL_0, 18.147f);
    set_C(CHANNEL_0, 100.0f);
    set_Q_factor(CHANNEL_0, 35.97f);

    if (set_FIN_FREF_DIV(CHANNEL_0)) {
        return -1;
    }

    set_LC_stabilize_time(CHANNEL_0);
    set_conversion_time(CHANNEL_0, 0x0546);
    set_driver_current(CHANNEL_0, 0xa000);

    /* single conversion */
    set_mux_config(0x20c);

    /* start channel 0 */
    u16 config = 0x1601;
    select_channel_to_convert(CHANNEL_0, &config);
    set_sensor_config(config);
    return NO_ERROR;
}

s32 LDC1612::LDC1612_mutiple_channel_config() {
    /* 20 TURNS */
    set_Rp(CHANNEL_0, 15.727f);
    set_L(CHANNEL_0, 20.0f);
    set_C(CHANNEL_0, 1200.0f);
    set_Q_factor(CHANNEL_0, 35.97f);

    set_Rp(CHANNEL_1, 15.727f);
    set_L(CHANNEL_1, 20.0f);
    set_C(CHANNEL_1, 1200.0f);
    set_Q_factor(CHANNEL_1, 35.97f);

    if (set_FIN_FREF_DIV(CHANNEL_0)) {
        return -1;
    }
    set_FIN_FREF_DIV(CHANNEL_1);

    /* 16*38/Fref = 30us — wait for LC sensor to stabilise before conversion */
    set_LC_stabilize_time(CHANNEL_0);
    set_LC_stabilize_time(CHANNEL_1);

    set_conversion_time(CHANNEL_0, 20000);
    set_conversion_time(CHANNEL_1, 20000);

    set_driver_current(CHANNEL_0, 0xa000);
    set_driver_current(CHANNEL_1, 0xa000);

    /* multiple conversion */
    set_mux_config(0x820c);
    /* This sets bit 12 (Sensor RP Override Enable) and sets AUTO_AMP_DIS (which is good for precision)*/
    set_sensor_config(0x1601);
    return NO_ERROR;
}

s32 LDC1612::parse_result_data(u8 channel, u32 raw_result, u32* result) {
    *result = raw_result & 0x0fffffff;
    if (0x0fffffff == *result) {
        fprintf(stderr, "can't detect coil inductance!\n");
        *result = 0;
        return -1;
    }
    u8 flags = (u8)(raw_result >> 24);
    if (flags & 0x80) fprintf(stdout, "channel %u: ERR_UR - under range error\n", channel);
    if (flags & 0x40) fprintf(stdout, "channel %u: ERR_OR - over range error\n",  channel);
    if (flags & 0x20) fprintf(stdout, "channel %u: ERR_WD - watchdog timeout\n",  channel);
    if (flags & 0x10) fprintf(stdout, "channel %u: ERR_AE - amplitude error\n",   channel);
    return NO_ERROR;
}

s32 LDC1612::get_channel_result(u8 channel, u32* result) {
    if (!result) {
        return ERROR_PARAM;
    }
    u16 value = 0;
    u32 raw_value = 0;
    IIC_read_16bit(CONVERTION_RESULT_REG_START + channel * 2,     &value);
    raw_value |= (u32)value << 16;
    IIC_read_16bit(CONVERTION_RESULT_REG_START + channel * 2 + 1, &value);
    raw_value |= (u32)value;
    return parse_result_data(channel, raw_value, result);
}
/* This function sets RCOUNT[channel], which results in the conversion time
 of tco = (RCOUNT * 16 / Fref0),
 where in turn Fref0 = FCLK / FREF_DIVIDER0, and is set by set_FIN_FREF_DIV()*/
s32 LDC1612::set_conversion_time(u8 channel, u16 value) {
    u32 actual_Fref = 40E6 / channel_params[channel].FREF_DIV;
    double tco = (double)(value * 16) / (double)actual_Fref * 1E6;
    printf("Conversion time for channel %u = %.1f us (%.1f ms)\n", channel, tco, tco / 1000);
    return IIC_write_16bit(SET_CONVERSION_TIME_REG_START + channel, value);
}

s32 LDC1612::set_conversion_offset(u8 channel, u16 value) {
    return IIC_write_16bit(SET_CONVERSION_OFFSET_REG_START + channel, value);
}

s32 LDC1612::set_LC_stabilize_time(u8 channel) {
    return IIC_write_16bit(SET_LC_STABILIZE_REG_START + channel, 30);
}

/* This function sets the FREF_DIVIDER[channel], which results in 
 ƒREF[channel] = ƒCLK / FREF_DIVIDER[channel] */
s32 LDC1612::set_FIN_FREF_DIV(u8 channel) {
    u16 FIN_DIV, FREF_DIV;

    Fsensor[channel] = 1.0f / (2.0f * (float)M_PI *
        sqrtf(inductance[channel] * capacitance[channel] * 1e-18f)) * 1e-6f;
    printf("fsensor[%u] = %f MHz\n", channel, Fsensor[channel]);

    FIN_DIV = (u16)(Fsensor[channel] / 8.75f + 1.0f);

    if (Fsensor[channel] * 4.0f < 40.0f) {
        FREF_DIV = 2;
        Fref[channel] = 40.0f / 2.0f;
    } else {
        FREF_DIV = 4;
        Fref[channel] = 40.0f / 4.0f;
    }

    u16 value = (FIN_DIV << 12) | FREF_DIV;
    printf("FIN_DIV[%u] = %u, FREF_DIV[%u] = %u\n", channel, FIN_DIV, channel, FREF_DIV);

    channel_params[channel].FIN_DIV = FIN_DIV;
    channel_params[channel].FREF_DIV = FREF_DIV;
    return IIC_write_16bit(SET_FREQ_REG_START + channel, value);
}

s32 LDC1612::set_ERROR_CONFIG(u16 value) {
    return IIC_write_16bit(ERROR_CONFIG_REG, value);
}

s32 LDC1612::set_mux_config(u16 value) {
    return IIC_write_16bit(MUL_CONFIG_REG, value);
}

s32 LDC1612::reset_sensor() {
    return IIC_write_16bit(SENSOR_RESET_REG, 0x8000);
}

s32 LDC1612::set_driver_current(u8 channel, u16 value) {
    return IIC_write_16bit(SET_DRIVER_CURRENT_REG + channel, value);
}

s32 LDC1612::set_sensor_config(u16 value) {
    return IIC_write_16bit(SENSOR_CONFIG_REG, value);
}

void LDC1612::select_channel_to_convert(u8 channel, u16* value) {
    switch (channel) {
        case 0: *value &= 0x3fff; break;
        case 1: *value  = (*value & 0x7fff) | 0x4000; break;
        case 2: *value  = (*value & 0xbfff) | 0x8000; break;
        case 3: *value |= 0xc000; break;
    }
}

void LDC1612::set_Rp(u8 channel, float n_kom)  { resistance[channel]  = n_kom; }
void LDC1612::set_L(u8 channel,  float n_uh)   { inductance[channel]  = n_uh;  }
void LDC1612::set_C(u8 channel,  float n_pf)   { capacitance[channel] = n_pf;  }
void LDC1612::set_Q_factor(u8 channel, float q) { Q_factor[channel]   = q;     }


static const char* status_str[] = {
    "conversion under range error",
    "conversion over range error",
    "watchdog timeout error",
    "amplitude high error",
    "amplitude low error",
    "zero count error",
    "data ready",
    "unread conversion present for channel 0",
    "unread conversion present for channel 1",
    "unread conversion present for channel 2",
    "unread conversion present for channel 3",
};

s32 LDC1612::sensor_status_parse(u16 value) {
    u16 section = value >> 14;
    printf("Channel %u is source of flag or error.\n", section);

    for (u32 i = 0; i < 6; i++) {
        if (value & ((u16)1 << (8 + i))) {
            printf("%s\n", status_str[6 - i]);
        }
    }
    if (value & (1 << 6)) {
        printf("%s\n", status_str[6]);
    }
    for (u32 i = 0; i < 4; i++) {
        if (value & (1 << i)) {
            printf("%s\n", status_str[10 - i]);
        }
    }
    return NO_ERROR;
}

u32 LDC1612::get_sensor_status() {
    u16 value = 0;
    IIC_read_16bit(SENSOR_STATUS_REG, &value);
    sensor_status_parse(value);
    return value;
}
