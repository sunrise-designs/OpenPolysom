#ifndef _LDC1612_H
#define _LDC1612_H

#include <Arduino.h>
#include <Wire.h>

typedef int32_t  s32;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t  u8;

typedef enum {
    NO_ERROR    =  0,
    ERROR_PARAM = -1,
    ERROR_COMM  = -2,
} ldc_err_t;

#define DEFAULT_IIC_ADDR                0x2B

#define CONVERTION_RESULT_REG_START     0x00
#define SET_CONVERSION_TIME_REG_START   0x08
#define SET_CONVERSION_OFFSET_REG_START 0x0C
#define SET_LC_STABILIZE_REG_START      0x10
#define SET_FREQ_REG_START              0x14
#define SENSOR_STATUS_REG               0x18
#define ERROR_CONFIG_REG                0x19
#define SENSOR_CONFIG_REG               0x1A
#define MUL_CONFIG_REG                  0x1B
#define SENSOR_RESET_REG                0x1C
#define SET_DRIVER_CURRENT_REG          0x1E
#define READ_MANUFACTURER_ID            0x7E
#define READ_DEVICE_ID                  0x7F

#define CHANNEL_0   0
#define CHANNEL_1   1
#define CHANNEL_NUM 2

class LDC1612 {
public:
    LDC1612(u8 addr = DEFAULT_IIC_ADDR);

    bool begin(TwoWire *wire = &Wire);
    void read_sensor_information();

    s32  get_channel_result(u8 channel, u32 *result);
    s32  LDC1612_mutiple_channel_config();
    s32  set_conversion_time(u8 channel, u16 value);
    s32  set_LC_stabilize_time(u8 channel);
    s32  set_conversion_offset(u8 channel, u16 value);
    s32  set_ERROR_CONFIG(u16 value);
    s32  set_sensor_config(u16 value);
    s32  set_mux_config(u16 value);
    s32  reset_sensor();
    s32  set_driver_current(u8 channel, u16 value);
    s32  set_FIN_FREF_DIV(u8 channel);
    void select_channel_to_convert(u8 channel, u16 *value);
    u32  get_sensor_status();

    void set_Rp(u8 channel, float n_kom);
    void set_L(u8 channel,  float n_uh);
    void set_C(u8 channel,  float n_pf);
    void set_Q_factor(u8 channel, float q);

    void IIC_read_16bit(u8 reg, u16 *value);  // public for ID reads

private:
    void write16(u8 reg, u16 value);
    u16  read16(u8 reg);
    s32  parse_result_data(u8 channel, u32 raw, u32 *result);
    s32  sensor_status_parse(u16 value);

    TwoWire *_wire;
    u8       _addr;

    float resistance[CHANNEL_NUM];
    float inductance[CHANNEL_NUM];
    float capacitance[CHANNEL_NUM];
    float Q_factor[CHANNEL_NUM];
    float Fsensor[CHANNEL_NUM];
    float Fref[CHANNEL_NUM];
    u16   FREF_DIV[CHANNEL_NUM];
    u16   FIN_DIV[CHANNEL_NUM];
};

#endif
