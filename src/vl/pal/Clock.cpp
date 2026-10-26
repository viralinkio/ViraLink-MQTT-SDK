#include "vl/pal/Clock.h"

#include "vl/Config.h"

#if VL_PLATFORM_ESP32
#include <sys/time.h>

#include "esp_timer.h"
#elif defined(ARDUINO)
#include <Arduino.h>
#else
#include <chrono>
#endif

namespace vl {

namespace {

#if VL_PLATFORM_ESP32

class Esp32Clock : public IClock {
 public:
  uint64_t uptimeMs() override { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }
  bool utcMs(uint64_t& out) override {
    struct timeval tv;
    if (gettimeofday(&tv, nullptr) != 0 || tv.tv_sec < static_cast<time_t>(VL_TIME_FLOOR_S)) return false;
    out = static_cast<uint64_t>(tv.tv_sec) * 1000u + static_cast<uint64_t>(tv.tv_usec / 1000);
    return true;
  }
};
typedef Esp32Clock PlatformClock;

#elif defined(ARDUINO)

class ArduinoClock : public IClock {
 public:
  uint64_t uptimeMs() override { return ext_.extend(millis()); }

 private:
  WrapSafeMillis ext_;
};
typedef ArduinoClock PlatformClock;

#else

class HostClock : public IClock {
 public:
  uint64_t uptimeMs() override {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
  }
  bool utcMs(uint64_t& out) override {
    using namespace std::chrono;
    out = static_cast<uint64_t>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    return true;
  }
};
typedef HostClock PlatformClock;

#endif

}  // namespace

IClock& platformClock() {
  static PlatformClock clock;
  return clock;
}

}  // namespace vl
