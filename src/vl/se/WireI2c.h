#pragma once

#if defined(ARDUINO)

#include <Arduino.h>
#include <Wire.h>

#include "vl/pal/I2c.h"

namespace vl {

// II2c over an Arduino TwoWire bus, already started with begin(). Other
// devices may share the bus as long as they are used from the same task.
class WireI2c : public II2c {
 public:
  // busHz: the bus speed restored after a wake pulse, which runs at 100 kHz.
  explicit WireI2c(TwoWire& wire, uint32_t busHz = 100000) : wire_(wire), busHz_(busHz) {}

  bool write(uint8_t address, const uint8_t* p, size_t n) override {
    wire_.beginTransmission(address);
    wire_.write(p, n);
    return wire_.endTransmission() == 0;
  }
  bool read(uint8_t address, uint8_t* buf, size_t n) override {
    size_t got = wire_.requestFrom(address, static_cast<uint8_t>(n));
    for (size_t i = 0; i < got; ++i) {
      int b = wire_.read();
      if (i < n) buf[i] = static_cast<uint8_t>(b);
    }
    return got == n;
  }
  // Addressing 0x00 at 100 kHz holds SDA low for 80 us; nobody answers.
  void wakePulse() override {
    wire_.setClock(100000);
    wire_.beginTransmission(static_cast<uint8_t>(0));
    wire_.endTransmission();
    wire_.setClock(busHz_);
  }
  void delayUs(uint32_t us) override {
    if (us >= 1000) delay(us / 1000);
    delayMicroseconds(us % 1000);
  }

 private:
  TwoWire& wire_;
  uint32_t busHz_;
};

}  // namespace vl

#endif  // ARDUINO
