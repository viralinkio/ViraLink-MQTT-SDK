#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vl/flash/Flash.h"
#include "vl/pal/OutboxStore.h"

namespace vl {

// IOutboxStore on NOR flash (a W25qFlash, or a FlashRegion of one): each
// segment is segmentSize bytes of whole erase blocks from the start of the
// flash. Fewer segments are reported when fewer fit, none when segmentSize is
// not a multiple of the erase size. sync() has nothing to do: program() is
// durable when it returns.
class FlashOutboxStore : public IOutboxStore {
 public:
  explicit FlashOutboxStore(IFlash& flash, size_t segments = 8, size_t segmentSize = 4096)
      : flash_(flash), segments_(segments), size_(segmentSize) {}

  size_t segmentCount() const override;
  size_t segmentSize() const override { return size_; }
  Result read(size_t seg, size_t off, uint8_t* buf, size_t n, size_t& got) override;
  Result write(size_t seg, size_t off, const uint8_t* p, size_t n) override;
  Result erase(size_t seg) override;
  Result sync(size_t seg) override;

 private:
  uint32_t at(size_t seg, size_t off) const;

  IFlash& flash_;
  size_t segments_;
  size_t size_;
};

}  // namespace vl
