#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vl {

// One device on an SPI bus, with its own chip select. Used on one task at a
// time; other devices may share the bus from that task.
class ISpiDevice {
 public:
  virtual ~ISpiDevice() {}
  // Applies the device's bus settings and pulls its chip select low.
  virtual void select() = 0;
  virtual void deselect() = 0;
  virtual void write(const uint8_t* p, size_t n) = 0;
  // Clocks out 0xFF while reading.
  virtual void read(uint8_t* buf, size_t n) = 0;
};

}  // namespace vl
