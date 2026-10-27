#pragma once

#if defined(ARDUINO)

#include <Arduino.h>
#include <SPI.h>
#include <string.h>

#include "vl/pal/Spi.h"

namespace vl {

// ISpiDevice over an Arduino SPIClass the application has started
// (SPI.begin()), with a transaction per command so other devices (a W5500)
// can share the bus. The chip-select pin is set up on first use.
class ArduinoSpi : public ISpiDevice {
 public:
  ArduinoSpi(SPIClass& spi, uint8_t csPin, uint32_t hz = 8000000) : spi_(spi), cs_(csPin), hz_(hz) {}

  void select() override {
    if (!pinReady_) {
      pinMode(cs_, OUTPUT);
      pinReady_ = true;
    }
    spi_.beginTransaction(SPISettings(hz_, MSBFIRST, SPI_MODE0));
    digitalWrite(cs_, LOW);
  }
  void deselect() override {
    digitalWrite(cs_, HIGH);
    spi_.endTransaction();
  }
  void write(const uint8_t* p, size_t n) override {
    uint8_t buf[kChunk];
    while (n) {
      size_t k = n < kChunk ? n : kChunk;
      memcpy(buf, p, k);  // transfer() overwrites its buffer
      spi_.transfer(buf, k);
      p += k;
      n -= k;
    }
  }
  void read(uint8_t* buf, size_t n) override {
    memset(buf, 0xFF, n);
    while (n) {
      size_t k = n < kChunk ? n : kChunk;
      spi_.transfer(buf, k);
      buf += k;
      n -= k;
    }
  }

 private:
  static const size_t kChunk = 64;

  SPIClass& spi_;
  uint8_t cs_;
  uint32_t hz_;
  bool pinReady_ = false;
};

}  // namespace vl

#endif  // ARDUINO
