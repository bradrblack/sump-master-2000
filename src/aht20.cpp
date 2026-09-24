#include "aht20.h"

namespace {
const uint8_t STATUS_BUSY       = 0x80;
const uint8_t STATUS_CALIBRATED = 0x08;

// CRC-8, polynomial 0x31, initial value 0xFF (AHT20 datasheet).
uint8_t crc8(const uint8_t *data, size_t len) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}
}  // namespace

bool Aht20::readStatus(uint8_t &status) {
  if (_wire.requestFrom(_addr, (size_t)1) != 1) return false;
  status = _wire.read();
  return true;
}

bool Aht20::begin() {
  uint8_t status;
  if (!readStatus(status)) return false;
  if (status & STATUS_CALIBRATED) return true;

  _wire.beginTransmission(_addr);
  _wire.write(0xBE);  // initialize / calibrate
  _wire.write(0x08);
  _wire.write(0x00);
  if (_wire.endTransmission() != 0) return false;
  delay(10);
  return readStatus(status) && (status & STATUS_CALIBRATED);
}

bool Aht20::start() {
  _wire.beginTransmission(_addr);
  _wire.write(0xAC);  // trigger measurement
  _wire.write(0x33);
  _wire.write(0x00);
  return _wire.endTransmission() == 0;
}

Aht20::Result Aht20::read(float &tempC, float &humidity) {
  uint8_t buf[7];
  if (_wire.requestFrom(_addr, sizeof(buf)) != sizeof(buf)) return ERROR;
  for (uint8_t &b : buf) b = _wire.read();

  if (buf[0] & STATUS_BUSY) return BUSY;
  if (crc8(buf, 6) != buf[6]) return ERROR;

  uint32_t rawHum  = ((uint32_t)buf[1] << 12) | ((uint32_t)buf[2] << 4) | (buf[3] >> 4);
  uint32_t rawTemp = ((uint32_t)(buf[3] & 0x0F) << 16) | ((uint32_t)buf[4] << 8) | buf[5];
  humidity = rawHum * 100.0f / 1048576.0f;
  tempC    = rawTemp * 200.0f / 1048576.0f - 50.0f;
  return OK;
}
