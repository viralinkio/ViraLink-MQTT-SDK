#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vl {

// I2C bus master, as the secure-element driver uses it. Addresses are 7-bit.
// Used on one task at a time.
class II2c {
 public:
  virtual ~II2c() {}
  // false on NACK or a bus error.
  virtual bool write(uint8_t address, const uint8_t* p, size_t n) = 0;
  // false on NACK: an ATECC608 NACKs while it is executing a command.
  virtual bool read(uint8_t address, uint8_t* buf, size_t n) = 0;
  // Holds SDA low for at least 60 us, which wakes an ATECC608.
  virtual void wakePulse() = 0;
  virtual void delayUs(uint32_t us) = 0;
};

}  // namespace vl
