#pragma once

#include <Arduino.h>
#include <Wire.h>

// Minimal non-blocking AHT20 temperature/humidity driver. start() triggers a
// measurement; read() fetches it at least 80 ms later. Every reading is
// CRC-checked, so a garbled I2C transfer is reported as an error rather than
// returned as a plausible-looking value.
class Aht20 {
 public:
  enum Result { OK, BUSY, ERROR };

  explicit Aht20(TwoWire &wire = Wire, uint8_t addr = 0x38) : _wire(wire), _addr(addr) {}

  // True if the sensor answers and is calibrated (sends the calibration
  // command if it isn't). Needs ~40 ms after power-up.
  bool begin();
  // Triggers a measurement; false if the sensor didn't ACK.
  bool start();
  // Reads the triggered measurement. BUSY means it isn't finished yet.
  Result read(float &tempC, float &humidity);

 private:
  bool readStatus(uint8_t &status);

  TwoWire &_wire;
  uint8_t _addr;
};
