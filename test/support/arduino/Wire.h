// Arduino TwoWire, as far as vl::WireI2c calls it.
#pragma once

#include <Arduino.h>

class TwoWire {
 public:
  void beginTransmission(uint8_t) {}
  size_t write(const uint8_t*, size_t n) { return n; }
  uint8_t endTransmission() { return 0; }
  size_t requestFrom(uint8_t, uint8_t) { return 0; }
  int read() { return -1; }
  void setClock(uint32_t) {}
};
