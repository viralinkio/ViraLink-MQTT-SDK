#include "vl/core/Scheduler.h"

namespace vl {

Result Scheduler::every(uint32_t intervalMs, Task fn, void* ctx, uint64_t nowMs) {
  if (intervalMs == 0 || !fn) return Result::Rejected;
  for (size_t i = 0; i < VL_MAX_TASKS; ++i) {
    Entry& e = entries_[i];
    if (e.interval != 0) continue;
    e.interval = intervalMs;
    e.due = nowMs + intervalMs;
    e.fn = fn;
    e.ctx = ctx;
    return Result::Ok;
  }
  return Result::NoMemory;
}

void Scheduler::cancel(Task fn, void* ctx) {
  for (size_t i = 0; i < VL_MAX_TASKS; ++i) {
    if (entries_[i].fn == fn && entries_[i].ctx == ctx) entries_[i] = Entry();
  }
}

bool Scheduler::takeDue(uint64_t nowMs, Task& fn, void*& ctx) {
  for (size_t i = 0; i < VL_MAX_TASKS; ++i) {
    Entry& e = entries_[i];
    if (e.interval == 0 || nowMs < e.due) continue;
    uint64_t late = nowMs - e.due;
    e.due += (late / e.interval + 1) * e.interval;  // skip missed runs, keep phase
    fn = e.fn;
    ctx = e.ctx;
    return true;
  }
  return false;
}

size_t Scheduler::size() const {
  size_t n = 0;
  for (size_t i = 0; i < VL_MAX_TASKS; ++i) n += entries_[i].interval != 0;
  return n;
}

}  // namespace vl
