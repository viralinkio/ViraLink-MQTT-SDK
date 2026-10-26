#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vl/Result.h"

namespace vl {

// Backend for the persistent outbox: a fixed number of equal-size segments
// (files, NOR sectors, FRAM regions) that are appended to and erased whole.
// Used on the client task only.
class IOutboxStore {
 public:
  virtual ~IOutboxStore() {}
  virtual size_t segmentCount() const = 0;
  virtual size_t segmentSize() const = 0;
  // Reads up to n bytes at off; got is fewer at the end of the data (a
  // file's end). Erased flash reads as 0xFF.
  virtual Result read(size_t seg, size_t off, uint8_t* buf, size_t n, size_t& got) = 0;
  // Appends: off is never below anything written to seg since its last erase.
  virtual Result write(size_t seg, size_t off, const uint8_t* p, size_t n) = 0;
  virtual Result erase(size_t seg) = 0;
  // Makes the writes to seg durable. A power cut may lose or tear writes made
  // since the last sync, and nothing else.
  virtual Result sync(size_t seg) = 0;
};

}  // namespace vl
