#include "Adafruit_MMA8451.h"
#include <string.h>

Adafruit_MMA8451::Adafruit_MMA8451(int32_t id)
    : _sensorID(id), _addr(0), _wire(nullptr) {}

bool Adafruit_MMA8451::begin(uint8_t addr, TwoWire *theWire) {
  _addr = addr;
  _wire = theWire;

  if (readRegister8(MMA8451_REG_WHOAMI) != 0x1A)
    return false;

  // Reset
  writeRegister8(MMA8451_REG_CTRL_REG2, 0x40);
  while (readRegister8(MMA8451_REG_CTRL_REG2) & 0x40)
    delay(1);

  // Standby mode
  writeRegister8(MMA8451_REG_CTRL_REG1, 0x00);
  // High resolution
  writeRegister8(MMA8451_REG_CTRL_REG2, 0x02);
  // Enable portrait/landscape detection
  writeRegister8(MMA8451_REG_PL_CFG, 0x40);

  // Default 2g range, 800 Hz — caller can override with setRange/setDataRate
  writeRegister8(MMA8451_REG_XYZ_DATA_CFG, MMA8451_RANGE_2_G);
  writeRegister8(MMA8451_REG_CTRL_REG1,
                 (MMA8451_DATARATE_800_HZ << 3) | 0x01);  // ACTIVE

  return true;
}

void Adafruit_MMA8451::read() {
  _wire->beginTransmission(_addr);
  _wire->write(MMA8451_REG_OUT_X_MSB);
  _wire->endTransmission(false);
  _wire->requestFrom(_addr, (uint8_t)6);

  x = (int16_t)(((uint16_t)_wire->read() << 8) | _wire->read()) >> 2;
  y = (int16_t)(((uint16_t)_wire->read() << 8) | _wire->read()) >> 2;
  z = (int16_t)(((uint16_t)_wire->read() << 8) | _wire->read()) >> 2;

  uint8_t range_bits = readRegister8(MMA8451_REG_XYZ_DATA_CFG) & 0x03;
  float scale = (float)(4096 >> range_bits);  // 4096 / 2048 / 1024 for 2g/4g/8g
  x_g = (float)x / scale;
  y_g = (float)y / scale;
  z_g = (float)z / scale;
}

void Adafruit_MMA8451::setRange(mma8451_range_t range) {
  uint8_t ctrl = readRegister8(MMA8451_REG_CTRL_REG1);
  // Standby
  writeRegister8(MMA8451_REG_CTRL_REG1, ctrl & ~0x01);
  writeRegister8(MMA8451_REG_XYZ_DATA_CFG, (uint8_t)range & 0x03);
  // Restore
  writeRegister8(MMA8451_REG_CTRL_REG1, ctrl);
}

mma8451_range_t Adafruit_MMA8451::getRange(void) {
  return (mma8451_range_t)(readRegister8(MMA8451_REG_XYZ_DATA_CFG) & 0x03);
}

void Adafruit_MMA8451::setDataRate(mma8451_dataRate_t dataRate) {
  uint8_t ctrl = readRegister8(MMA8451_REG_CTRL_REG1);
  // Standby
  writeRegister8(MMA8451_REG_CTRL_REG1, ctrl & ~0x01);
  ctrl &= ~(MMA8451_DATARATE_MASK << 3);
  ctrl |= ((uint8_t)dataRate << 3);
  writeRegister8(MMA8451_REG_CTRL_REG1, ctrl | 0x01);  // restore ACTIVE
}

mma8451_dataRate_t Adafruit_MMA8451::getDataRate(void) {
  return (mma8451_dataRate_t)(
      (readRegister8(MMA8451_REG_CTRL_REG1) >> 3) & MMA8451_DATARATE_MASK);
}

uint8_t Adafruit_MMA8451::getOrientation(void) {
  return readRegister8(MMA8451_REG_PL_STATUS) & 0x07;
}

bool Adafruit_MMA8451::getEvent(sensors_event_t *event) {
  read();
  event->version   = sizeof(sensors_event_t);
  event->sensor_id = _sensorID;
  event->type      = SENSOR_TYPE_ACCELEROMETER;
  event->timestamp = millis();
  event->acceleration.x = x_g * SENSORS_GRAVITY_STANDARD;
  event->acceleration.y = y_g * SENSORS_GRAVITY_STANDARD;
  event->acceleration.z = z_g * SENSORS_GRAVITY_STANDARD;
  return true;
}

void Adafruit_MMA8451::getSensor(sensor_t *sensor) {
  memset(sensor, 0, sizeof(sensor_t));
  strncpy(sensor->name, "MMA8451", 12);
  sensor->version    = 1;
  sensor->sensor_id  = _sensorID;
  sensor->type       = SENSOR_TYPE_ACCELEROMETER;
  sensor->max_value  =  78.4f;   // +8g * 9.8 m/s²
  sensor->min_value  = -78.4f;
  sensor->resolution =  0.002f;
  sensor->min_delay  =  1250;    // 800 Hz → 1250 µs
}

void Adafruit_MMA8451::writeRegister8(uint8_t reg, uint8_t value) {
  _wire->beginTransmission(_addr);
  _wire->write(reg);
  _wire->write(value);
  _wire->endTransmission();
}

uint8_t Adafruit_MMA8451::readRegister8(uint8_t reg) {
  _wire->beginTransmission(_addr);
  _wire->write(reg);
  _wire->endTransmission(false);
  _wire->requestFrom(_addr, (uint8_t)1);
  return _wire->read();
}
