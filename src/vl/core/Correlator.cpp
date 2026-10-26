#include "vl/core/Correlator.h"

namespace vl {

uint32_t Correlator::add(uint64_t deadlineMs, Callback cb, void* ctx, const void* owner) {
  for (size_t i = 0; i < VL_MAX_PENDING; ++i) {
    Entry& e = entries_[i];
    if (e.id != 0) continue;
    e.id = nextId_++;
    if (nextId_ == 0) nextId_ = 1;
    e.deadline = deadlineMs;
    e.cb = cb;
    e.ctx = ctx;
    e.owner = owner;
    return e.id;
  }
  return 0;
}

bool Correlator::take(uint32_t id, Callback& cb, void*& ctx) {
  if (id == 0) return false;
  for (size_t i = 0; i < VL_MAX_PENDING; ++i) {
    Entry& e = entries_[i];
    if (e.id != id) continue;
    cb = e.cb;
    ctx = e.ctx;
    e = Entry();
    return true;
  }
  return false;
}

bool Correlator::takeExpired(uint64_t nowMs, Callback& cb, void*& ctx) {
  for (size_t i = 0; i < VL_MAX_PENDING; ++i) {
    Entry& e = entries_[i];
    if (e.id == 0 || nowMs < e.deadline) continue;
    cb = e.cb;
    ctx = e.ctx;
    e = Entry();
    return true;
  }
  return false;
}

bool Correlator::takeAny(Callback& cb, void*& ctx) {
  for (size_t i = 0; i < VL_MAX_PENDING; ++i) {
    Entry& e = entries_[i];
    if (e.id == 0) continue;
    cb = e.cb;
    ctx = e.ctx;
    e = Entry();
    return true;
  }
  return false;
}

size_t Correlator::removeOwner(const void* owner) {
  size_t n = 0;
  for (size_t i = 0; i < VL_MAX_PENDING; ++i) {
    Entry& e = entries_[i];
    if (e.id == 0 || !owner || e.owner != owner) continue;
    e = Entry();
    ++n;
  }
  return n;
}

size_t Correlator::pending() const {
  size_t n = 0;
  for (size_t i = 0; i < VL_MAX_PENDING; ++i) n += entries_[i].id != 0;
  return n;
}

}  // namespace vl
