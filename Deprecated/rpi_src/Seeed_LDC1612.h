/*
    Seeed_LDC1612.h
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

#ifndef _SEEED_LDC1612_H
#define _SEEED_LDC1612_H

#include <cstdint>
#include <cstdio>

typedef int32_t        s32;
typedef uint32_t       u32;
typedef int16_t        s16;
typedef uint16_t       u16;
typedef int8_t         s8;
typedef uint8_t        u8;

typedef enum {
    NO_ERROR    =    0,
    ERROR_PARAM =   -1,
    ERROR_COMM  =   -2,
    ERROR_OTHERS = -128,
} ldc_err_t;

#define CHECK_RESULT(a, b) do { if ((a) = (b)) { \
    fprintf(stderr, "%s:%d error code = %d\n", __FILE__, __LINE__, (a)); \
    return (a); \
} } while (0)

#define DEFAULT_IIC_ADDR  0x2B

/*Register addresses*/
#define CONVERTION_RESULT_REG_START         0x00
#define SET_CONVERSION_TIME_REG_START       0x08
#define SET_CONVERSION_OFFSET_REG_START     0x0C
#define SET_LC_STABILIZE_REG_START          0x10
#define SET_FREQ_REG_START                  0x14

#define SENSOR_STATUS_REG                   0x18
#define ERROR_CONFIG_REG                    0x19
#define SENSOR_CONFIG_REG                   0x1A
#define MUL_CONFIG_REG                      0x1B
#define SENSOR_RESET_REG                    0x1C
#define SET_DRIVER_CURRENT_REG              0x1E

#define READ_MANUFACTURER_ID                0x7E
#define READ_DEVICE_ID                      0x7F

/*ERROR bits*/
#define UR_ERR2OUT  ((u16)1 << 15)
#define OR_ERR2OUT  ((u16)1 << 14)
#define WD_ERR2OUT  ((u16)1 << 13)
#define AH_ERR2OUT  ((u16)1 << 12)
#define AL_ERR2OUT  ((u16)1 << 11)
#define UR_ERR2INT  ((u16)1 <<  7)
#define OR_ERR2INT  ((u16)1 <<  6)
#define WD_ERR2INT  ((u16)1 <<  5)
#define AH_ERR2INT  ((u16)1 <<  4)
#define AL_ERR2INT  ((u16)1 <<  3)
#define ZC_ERR2INT  ((u16)1 <<  2)
#define DRDY_2INT   ((u16)1 <<  0)

/*SENSOR CONFIG bits*/
#define ACTIVE_CHANNEL       (((u16)1 << 15) | ((u16)1 << 14))
#define SLEEP_MODE_EN        ((u16)1 << 13)
#define RP_OVERRIDE_EN       ((u16)1 << 12)
#define SENSOR_ACTIVATE_SEL  ((u16)1 << 11)
#define AUTO_AMP_DIS         ((u16)1 << 10)
#define REF_CLK_SRC          ((u16)1 <<  9)
#define INTB_DIS             ((u16)1 <<  7)
#define HIGH_CURRENT_DRV     ((u16)1 <<  6)
/* Low bits 0-5 are reserved; must be set to 000001 */

#define CHANNEL_0   0
#define CHANNEL_1   1
#define CHANNEL_NUM 2


class LDC1612_IIC_OPRTS {
  public:
    LDC1612_IIC_OPRTS() : _IIC_ADDR(DEFAULT_IIC_ADDR), _i2c_fd(-1) {}
    ~LDC1612_IIC_OPRTS();

    bool IIC_begin(const char* i2c_dev = "/dev/i2c-1");
    void IIC_close();
    void set_iic_addr(u8 IIC_ADDR);

    s32  IIC_write_byte(u8 reg, u8 byte);
    s32  IIC_write_16bit(u8 reg, u16 value);
    void IIC_read_byte(u8 reg, u8* byte);
    void IIC_read_16bit(u8 start_reg, u16* value);

  private:
    u8  _IIC_ADDR;
    int _i2c_fd;
};


class LDC1612 : public LDC1612_IIC_OPRTS {
  public:
    LDC1612(u8 IIC_ADDR = DEFAULT_IIC_ADDR);
    ~LDC1612() {}

    s32  init(const char* i2c_dev = "/dev/i2c-1");
    void read_sensor_infomation();
    s32  get_channel_result(u8 channel, u32* result);
    s32  set_conversion_time(u8 channel, u16 value);
    s32  set_LC_stabilize_time(u8 channel);
    s32  set_conversion_offset(u8 channel, u16 value);
    u32  get_sensor_status();
    s32  set_ERROR_CONFIG(u16 value);
    s32  set_sensor_config(u16 value);
    s32  set_mux_config(u16 value);
    s32  reset_sensor();
    s32  set_driver_current(u8 channel, u16 value);
    s32  set_FIN_FREF_DIV(u8 channel);
    void select_channel_to_convert(u8 channel, u16* value);
    s32  single_channel_config(u8 channel);
    s32  LDC1612_mutiple_channel_config();

    void set_Rp(u8 channel, float n_kom);
    void set_L(u8 channel, float n_uh);
    void set_C(u8 channel, float n_pf);
    void set_Q_factor(u8 channel, float q);

  private:
    s32 parse_result_data(u8 channel, u32 raw_result, u32* result);
    s32 sensor_status_parse(u16 value);

    float resistance[CHANNEL_NUM];
    float inductance[CHANNEL_NUM];
    float capacitance[CHANNEL_NUM];
    float Fref[CHANNEL_NUM];
    float Fsensor[CHANNEL_NUM];
    float Q_factor[CHANNEL_NUM];
};

#endif
