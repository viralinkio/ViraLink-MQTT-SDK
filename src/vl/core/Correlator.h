#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vl/Config.h"
#include "vl/Result.h"

namespace vl {

// Fixed table of outstanding requests (attribute requests, client RPC,
// provisioning, OTA chunks). Every entry ends exactly once: resolved with the
// response, or fired with Result::Timeout at its deadline.
class Correlator {
 public:
  // On Ok, p/n is the response payload (valid only during the call).
  typedef void (*Callback)(Result r, const uint8_t* p, size_t n, void* ctx);

  // Returns the request id (never 0), or 0 when the table is full. owner
  // tags the entry so removeOwner() can drop it.
  uint32_t add(uint64_t deadlineMs, Callback cb, void* ctx, const void* owner = nullptr);
  // Removes the entry for id and fills cb/ctx. False if unknown or expired.
  bool take(uint32_t id, Callback& cb, void*& ctx);
  // Removes one expired entry. Call until it returns false; the caller fires
  // the callback with Result::Timeout outside any lock.
  bool takeExpired(uint64_t nowMs, Callback& cb, void*& ctx);
  // Removes any entry (used to fail everything on shutdown).
  bool takeAny(Callback& cb, void*& ctx);
  // Removes every entry of owner without firing it; returns how many.
  size_t removeOwner(const void* owner);

  size_t pending() const;
  static size_t capacity() { return VL_MAX_PENDING; }

 private:
  struct Entry {
    uint64_t deadline = 0;
    uint32_t id = 0;  // 0 = free
    Callback cb = nullptr;
    void* ctx = nullptr;
    const void* owner = nullptr;
  };
  Entry entries_[VL_MAX_PENDING];
  uint32_t nextId_ = 1;
};

}  // namespace vl
