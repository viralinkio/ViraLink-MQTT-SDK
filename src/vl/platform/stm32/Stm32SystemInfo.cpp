#include "vl/Config.h"

#if VL_PLATFORM_STM32

#include <Arduino.h>
#include <malloc.h>

#include "vl/pal/SystemInfo.h"

extern "C" void* _sbrk(ptrdiff_t incr);

namespace vl {

namespace {

// Flag names differ between families, hence a check per flag. The order
// matters: a power-on also sets the pin and brownout flags.
const char* readResetReason() {
  const char* r = nullptr;
#if defined(RCC_FLAG_IWDGRST)
  if (!r && __HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST)) r = "wdt";
#endif
#if defined(RCC_FLAG_IWDG1RST)
  if (!r && __HAL_RCC_GET_FLAG(RCC_FLAG_IWDG1RST)) r = "wdt";
#endif
#if defined(RCC_FLAG_WWDGRST)
  if (!r && __HAL_RCC_GET_FLAG(RCC_FLAG_WWDGRST)) r = "wdt";
#endif
#if defined(RCC_FLAG_WWDG1RST)
  if (!r && __HAL_RCC_GET_FLAG(RCC_FLAG_WWDG1RST)) r = "wdt";
#endif
#if defined(RCC_FLAG_SFTRST)
  if (!r && __HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST)) r = "software";
#endif
#if defined(RCC_FLAG_PORRST)
  if (!r && __HAL_RCC_GET_FLAG(RCC_FLAG_PORRST)) r = "power_on";
#endif
#if defined(RCC_FLAG_PWRRST)
  if (!r && __HAL_RCC_GET_FLAG(RCC_FLAG_PWRRST)) r = "power_on";
#endif
#if defined(RCC_FLAG_BORRST)
  if (!r && __HAL_RCC_GET_FLAG(RCC_FLAG_BORRST)) r = "brownout";
#endif
#if defined(RCC_FLAG_PINRST)
  if (!r && __HAL_RCC_GET_FLAG(RCC_FLAG_PINRST)) r = "external";
#endif
  __HAL_RCC_CLEAR_RESET_FLAGS();  // so the next reset is told apart
  return r ? r : "unknown";
}

class Stm32SystemInfo : public ISystemInfo {
 public:
  // The gap between the heap break and the stack, plus what malloc holds free.
  bool heapFree(uint32_t& bytes) override {
    char* brk = static_cast<char*>(_sbrk(0));
    char* sp = reinterpret_cast<char*>(__get_MSP());
    bytes = static_cast<uint32_t>((sp > brk ? sp - brk : 0) + mallinfo().fordblks);
    return true;
  }
  const char* resetReason() override {
    if (!reason_) reason_ = readResetReason();
    return reason_;
  }

 private:
  const char* reason_ = nullptr;
};

}  // namespace

ISystemInfo& platformSystemInfo() {
  static Stm32SystemInfo info;
  return info;
}

}  // namespace vl

#endif  // VL_PLATFORM_STM32
