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

// esp_timer (ESP32) or micros64() (ESP8266) with gettimeofday(), a wrap-safe
// millis() extension on other Arduino targets, std::chrono on a host build.
IClock& platformClock();

// Sets the wall clock from a source the application has: NTP over UDP, a
// modem (AT+CCLK), an RTC. ESP32 and ESP8266 set the system time; elsewhere
// the platform clock keeps the offset from uptime. False, and nothing set,
// for a time before VL_TIME_FLOOR_S. Call from the client task or before
// begin(): the offset is not atomic.
bool setUtcMs(uint64_t utcMs);

// Wall-clock time kept as an offset from uptime.
class UtcOffset {
 public:
  void set(uint64_t utcMs, uint64_t uptimeMs) {
    offset_ = utcMs - uptimeMs;
    set_ = true;
  }
  bool get(uint64_t uptimeMs, uint64_t& out) const {
    if (!set_) return false;
    out = uptimeMs + offset_;
    return true;
  }

 private:
  uint64_t offset_ = 0;
  bool set_ = false;
};

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
