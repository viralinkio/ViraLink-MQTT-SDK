#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vl/Result.h"

namespace vl {

// NOR flash as the library uses it: erased bytes read 0xFF, programming can
// only clear bits, and erasing works on whole blocks. Used on one task at a
// time.
class IFlash {
 public:
  virtual ~IFlash() {}
  // Bytes; 0 while the flash is not ready (a chip not yet identified).
  virtual uint32_t size() const = 0;
  // Erase block, a power of two.
  virtual uint32_t eraseSize() const = 0;
  virtual Result read(uint32_t addr, uint8_t* buf, size_t n) = 0;
  // Programs n bytes at addr, which should be erased, crossing pages as
  // needed. Returns once the bytes are durable.
  virtual Result program(uint32_t addr, const uint8_t* p, size_t n) = 0;
  // Erases the block that starts at addr.
  virtual Result erase(uint32_t addr) = 0;
};

// The window [offset, offset + size) of another flash, so one chip can hold
// the outbox, the key-value store and an OTA slot. offset and size must be
// multiples of the erase size and fit in the flash; otherwise the region has
// size 0 and refuses every access. Checked on each call, because a chip's
// size is known only once it has been identified.
class FlashRegion : public IFlash {
 public:
  FlashRegion(IFlash& flash, uint32_t offset, uint32_t size) : flash_(flash), offset_(offset), size_(size) {}

  uint32_t size() const override;
  uint32_t eraseSize() const override { return flash_.eraseSize(); }
  Result read(uint32_t addr, uint8_t* buf, size_t n) override;
  Result program(uint32_t addr, const uint8_t* p, size_t n) override;
  Result erase(uint32_t addr) override;

 private:
  bool inside(uint32_t addr, size_t n) const;

  IFlash& flash_;
  uint32_t offset_;
  uint32_t size_;
};

}  // namespace vl
