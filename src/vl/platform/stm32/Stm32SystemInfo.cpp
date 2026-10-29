#include "vl/Config.h"

#if VL_PLATFORM_STM32

#include <Arduino.h>

#include "vl/pal/SystemInfo.h"

namespace vl {

namespace {

// The RCC reset flags accumulate until cleared, so the first read keeps the
// answer and clears them; the next boot then reports only its own cause.
// Heap and link values are left to an ISystemInfo of the application's.
class Stm32SystemInfo : public ISystemInfo {
 public:
  const char* resetReason() override {
    if (!reason_) {
      reason_ = read();
      __HAL_RCC_CLEAR_RESET_FLAGS();
    }
    return reason_;
  }

 private:
  static bool flag(uint32_t f) { return __HAL_RCC_GET_FLAG(f) != 0U; }
  static const char* read() {
#ifdef RCC_FLAG_PORRST
    if (flag(RCC_FLAG_PORRST)) return "power_on";  // a power-on also sets BORRST
#endif
#ifdef RCC_FLAG_BORRST
    if (flag(RCC_FLAG_BORRST)) return "brownout";
#endif
#ifdef RCC_FLAG_IWDGRST
    if (flag(RCC_FLAG_IWDGRST)) return "wdt";
#endif
#ifdef RCC_FLAG_WWDGRST
    if (flag(RCC_FLAG_WWDGRST)) return "wdt";
#endif
#ifdef RCC_FLAG_SFTRST
    if (flag(RCC_FLAG_SFTRST)) return "software";
#endif
#ifdef RCC_FLAG_PINRST
    if (flag(RCC_FLAG_PINRST)) return "external";
#endif
    return "unknown";
  }

  const char* reason_ = nullptr;
};

}  // namespace

ISystemInfo& platformSystemInfo() {
  static Stm32SystemInfo info;
  return info;
}

}  // namespace vl

#endif  // VL_PLATFORM_STM32
