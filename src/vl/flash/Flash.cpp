#include "vl/flash/Flash.h"

namespace vl {

uint32_t FlashRegion::size() const {
  uint32_t block = flash_.eraseSize();
  uint32_t total = flash_.size();
  if (block == 0 || offset_ % block || size_ % block || offset_ > total || size_ > total - offset_) return 0;
  return size_;
}

bool FlashRegion::inside(uint32_t addr, size_t n) const {
  uint32_t s = size();
  return s && addr <= s && n <= s - addr;
}

Result FlashRegion::read(uint32_t addr, uint8_t* buf, size_t n) {
  if (!inside(addr, n)) return Result::Rejected;
  return flash_.read(offset_ + addr, buf, n);
}

Result FlashRegion::program(uint32_t addr, const uint8_t* p, size_t n) {
  if (!inside(addr, n)) return Result::Rejected;
  return flash_.program(offset_ + addr, p, n);
}

Result FlashRegion::erase(uint32_t addr) {
  if (!inside(addr, 1) || addr % flash_.eraseSize()) return Result::Rejected;
  return flash_.erase(offset_ + addr);
}

}  // namespace vl
