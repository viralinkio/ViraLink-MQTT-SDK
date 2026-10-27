#include "vl/flash/FlashOutboxStore.h"

namespace vl {

size_t FlashOutboxStore::segmentCount() const {
  uint32_t block = flash_.eraseSize();
  if (block == 0 || size_ == 0 || size_ % block) return 0;
  uint32_t fit = flash_.size() / size_;
  return fit < segments_ ? static_cast<size_t>(fit) : segments_;
}

// size_t is 16 bits on AVR, so addresses are computed in 32.
uint32_t FlashOutboxStore::at(size_t seg, size_t off) const { return static_cast<uint32_t>(seg) * size_ + off; }

Result FlashOutboxStore::read(size_t seg, size_t off, uint8_t* buf, size_t n, size_t& got) {
  got = 0;
  if (seg >= segmentCount() || off > size_) return Result::Rejected;
  size_t k = n < size_ - off ? n : size_ - off;
  if (k == 0) return Result::Ok;
  Result r = flash_.read(at(seg, off), buf, k);
  if (r == Result::Ok) got = k;
  return r;
}

Result FlashOutboxStore::write(size_t seg, size_t off, const uint8_t* p, size_t n) {
  if (seg >= segmentCount() || off > size_ || n > size_ - off) return Result::Rejected;
  return flash_.program(at(seg, off), p, n);
}

Result FlashOutboxStore::erase(size_t seg) {
  if (seg >= segmentCount()) return Result::Rejected;
  uint32_t block = flash_.eraseSize();
  for (size_t off = 0; off < size_; off += block) {
    Result r = flash_.erase(at(seg, off));
    if (r != Result::Ok) return r;
  }
  return Result::Ok;
}

Result FlashOutboxStore::sync(size_t seg) { return seg < segmentCount() ? Result::Ok : Result::Rejected; }

}  // namespace vl
