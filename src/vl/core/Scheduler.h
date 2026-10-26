#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vl/Config.h"
#include "vl/Result.h"

namespace vl {

// Periodic tasks run from the client loop, timed on monotonic uptime.
// Drift-free: the next run is the previous due time plus the interval. After
// a stall, missed runs are skipped rather than fired in a burst.
class Scheduler {
 public:
  typedef void (*Task)(void* ctx);

  // First run is one interval after nowMs. NoMemory when the table is full,
  // Rejected for a zero interval or null task.
  Result every(uint32_t intervalMs, Task fn, void* ctx, uint64_t nowMs);
  // Removes every entry with this fn/ctx pair.
  void cancel(Task fn, void* ctx);

  // Returns the next due task and advances its schedule; call until false.
  bool takeDue(uint64_t nowMs, Task& fn, void*& ctx);

  size_t size() const;

 private:
  struct Entry {
    uint64_t due = 0;
    uint32_t interval = 0;  // 0 = free
    Task fn = nullptr;
    void* ctx = nullptr;
  };
  Entry entries_[VL_MAX_TASKS];
};

}  // namespace vl
