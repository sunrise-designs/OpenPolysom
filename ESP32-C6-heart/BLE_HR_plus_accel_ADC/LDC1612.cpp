#include "LDC1612.h"
#include <math.h>

LDC1612::LDC1612(u8 addr) : _wire(nullptr), _addr(addr) {
    memset(resistance,  0, sizeof(resistance));
    memset(inductance,  0, sizeof(inductance));
    memset(capacitance, 0, sizeof(capacitance));
    memset(Q_factor,    0, sizeof(Q_factor));
    memset(Fsensor,     0, sizeof(Fsensor));
    memset(Fref,        0, sizeof(Fref));
    memset(FREF_DIV,    0, sizeof(FREF_DIV));
    memset(FIN_DIV,     0, sizeof(FIN_DIV));
}

bool LDC1612::begin(TwoWire *wire) {
    _wire = wire;
    u16 mfr = read16(READ_MANUFACTURER_ID);
    if (mfr != 0x5449) {
        Serial.printf("LDC1612: unexpected mfr 0x%04X (want 0x5449)\n", mfr);
        return false;
    }
    return true;
}

void LDC1612::write16(u8 reg, u16 value) {
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    _wire->write((uint8_t)(value >> 8));
    _wire->write((uint8_t)(value & 0xFF));
    _wire->endTransmission();
}

u16 LDC1612::read16(u8 reg) {
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    _wire->endTransmission(false);
    _wire->requestFrom(_addr, (uint8_t)2);
    uint8_t hi = _wire->read();
    uint8_t lo = _wire->read();
    return ((u16)hi << 8) | lo;
}

void LDC1612::IIC_read_16bit(u8 reg, u16 *value) {
    *value = read16(reg);
}

void LDC1612::read_sensor_information() {
    Serial.printf("LDC1612 mfr=0x%04X  dev=0x%04X\n",
                  read16(READ_MANUFACTURER_ID), read16(READ_DEVICE_ID));
}

s32 LDC1612::get_channel_result(u8 channel, u32 *result) {
    if (!result) return ERROR_PARAM;
    u16 hi = read16(CONVERTION_RESULT_REG_START + channel * 2);
    u16 lo = read16(CONVERTION_RESULT_REG_START + channel * 2 + 1);
    return parse_result_data(channel, ((u32)hi << 16) | lo, result);
}

s32 LDC1612::parse_result_data(u8 channel, u32 raw, u32 *result) {
    *result = raw & 0x0FFFFFFF;
    if (*result == 0x0FFFFFFF) {
        Serial.printf("LDC1612 ch%u: coil not detected\n", channel);
        *result = 0;
        return -1;
    }
    u8 flags = (u8)(raw >> 24);
    if (flags & 0x08) Serial.printf("LDC1612 ch%u: ERR_UR\n", channel);
    if (flags & 0x04) Serial.printf("LDC1612 ch%u: ERR_OR\n", channel);
    if (flags & 0x02) Serial.printf("LDC1612 ch%u: ERR_WD\n", channel);
    if (flags & 0x01) Serial.printf("LDC1612 ch%u: ERR_AE\n", channel);
    return NO_ERROR;
}

s32 LDC1612::set_conversion_time(u8 channel, u16 value) {
    if (FREF_DIV[channel] == 0) return ERROR_PARAM;
    float tco_us = (float)value * 16.0f / (40e6f / FREF_DIV[channel]) * 1e6f;
    Serial.printf("LDC1612 ch%u: tco = %.1f us\n", channel, tco_us);
    write16(SET_CONVERSION_TIME_REG_START + channel, value);
    return NO_ERROR;
}

s32 LDC1612::set_conversion_offset(u8 channel, u16 value) {
    write16(SET_CONVERSION_OFFSET_REG_START + channel, value);
    return NO_ERROR;
}

s32 LDC1612::set_LC_stabilize_time(u8 channel) {
    write16(SET_LC_STABILIZE_REG_START + channel, 30);
    return NO_ERROR;
}

s32 LDC1612::set_FIN_FREF_DIV(u8 channel) {
    Fsensor[channel] = 1.0f / (2.0f * (float)M_PI *
        sqrtf(inductance[channel] * capacitance[channel] * 1e-18f)) * 1e-6f;
    Serial.printf("LDC1612 ch%u: Fsensor = %.4f MHz\n", channel, Fsensor[channel]);

    FIN_DIV[channel] = (u16)(Fsensor[channel] / 8.75f + 1.0f);

    if (Fsensor[channel] * 4.0f < 40.0f) {
        FREF_DIV[channel] = 2;
        Fref[channel]     = 20.0f;
    } else {
        FREF_DIV[channel] = 4;
        Fref[channel]     = 10.0f;
    }

    Serial.printf("LDC1612 ch%u: FIN_DIV=%u  FREF_DIV=%u\n",
                  channel, FIN_DIV[channel], FREF_DIV[channel]);
    write16(SET_FREQ_REG_START + channel,
            (u16)(FIN_DIV[channel] << 12) | FREF_DIV[channel]);
    return NO_ERROR;
}

s32 LDC1612::set_ERROR_CONFIG(u16 value) { write16(ERROR_CONFIG_REG,   value); return NO_ERROR; }
s32 LDC1612::set_mux_config(u16 value)   { write16(MUL_CONFIG_REG,     value); return NO_ERROR; }
s32 LDC1612::set_sensor_config(u16 value){ write16(SENSOR_CONFIG_REG,  value); return NO_ERROR; }
s32 LDC1612::reset_sensor()              { write16(SENSOR_RESET_REG,   0x8000); return NO_ERROR; }

s32 LDC1612::set_driver_current(u8 channel, u16 value) {
    write16(SET_DRIVER_CURRENT_REG + channel, value);
    return NO_ERROR;
}

void LDC1612::select_channel_to_convert(u8 channel, u16 *value) {
    switch (channel) {
        case 0: *value &= 0x3FFF; break;
        case 1: *value  = (*value & 0x7FFF) | 0x4000; break;
        case 2: *value  = (*value & 0xBFFF) | 0x8000; break;
        case 3: *value |= 0xC000; break;
    }
}

void LDC1612::set_Rp(u8 ch, float v)      { resistance[ch]  = v; }
void LDC1612::set_L(u8 ch,  float v)      { inductance[ch]  = v; }
void LDC1612::set_C(u8 ch,  float v)      { capacitance[ch] = v; }
void LDC1612::set_Q_factor(u8 ch, float v){ Q_factor[ch]    = v; }

s32 LDC1612::sensor_status_parse(u16 value) {
    Serial.printf("LDC1612 status (ch %u): ", (value >> 14) & 0x3);
    if (value & (1 << 13)) Serial.print("ERR_UR ");
    if (value & (1 << 12)) Serial.print("ERR_OR ");
    if (value & (1 << 11)) Serial.print("ERR_WD ");
    if (value & (1 << 10)) Serial.print("ERR_AH ");
    if (value & (1 <<  9)) Serial.print("ERR_AL ");
    if (value & (1 <<  8)) Serial.print("ERR_ZC ");
    if (value & (1 <<  6)) Serial.print("DRDY ");
    Serial.println();
    return NO_ERROR;
}

u32 LDC1612::get_sensor_status() {
    u16 value = read16(SENSOR_STATUS_REG);
    sensor_status_parse(value);
    return value;
}

s32 LDC1612::LDC1612_mutiple_channel_config() {
    set_Rp(CHANNEL_0, 15.727f);  set_L(CHANNEL_0, 20.0f);
    set_C(CHANNEL_0, 1200.0f);   set_Q_factor(CHANNEL_0, 35.97f);

    set_Rp(CHANNEL_1, 15.727f);  set_L(CHANNEL_1, 20.0f);
    set_C(CHANNEL_1, 1200.0f);   set_Q_factor(CHANNEL_1, 35.97f);

    if (set_FIN_FREF_DIV(CHANNEL_0)) return -1;
    set_FIN_FREF_DIV(CHANNEL_1);

    set_LC_stabilize_time(CHANNEL_0);
    set_LC_stabilize_time(CHANNEL_1);

    set_conversion_time(CHANNEL_0, 20000);
    set_conversion_time(CHANNEL_1, 20000);

    set_driver_current(CHANNEL_0, 0xa000);
    set_driver_current(CHANNEL_1, 0xa000);

    set_mux_config(0x820c);     // multiplexed, auto-scan both channels
    set_sensor_config(0x1601);  // RP_OVERRIDE_EN | AUTO_AMP_DIS | reserved=1
    return NO_ERROR;
}
