#pragma once

#include <stdint.h>

namespace vl {

// Time source. uptimeMs() is monotonic and 64-bit; it never wraps.
// utcMs() reports wall-clock time once a source (SNTP, cellular network time,
// RTC) has set it, and returns false before that.
class IClock {
 public:
  virtual ~IClock() {}
  virtual uint64_t uptimeMs() = 0;
  virtual bool utcMs(uint64_t& out) {
    (void)out;
    return false;
  }
};

// esp_timer + gettimeofday() on ESP32, a wrap-safe millis() extension on other
// Arduino targets, std::chrono on a host build.
IClock& platformClock();

// Extends a 32-bit millisecond counter to 64 bits. Must be sampled at least
// once per wrap period (49.7 days); the client loop does that.
class WrapSafeMillis {
 public:
  uint64_t extend(uint32_t now32) {
    if (now32 < last_) high_ += 1;
    last_ = now32;
    return (static_cast<uint64_t>(high_) << 32) | now32;
  }

 private:
  uint32_t last_ = 0;
  uint32_t high_ = 0;
};

}  // namespace vl
