#include "vl/pal/SystemInfo.h"

#include "vl/Config.h"

namespace vl {

#if !VL_PLATFORM_ESP32 && !VL_PLATFORM_ESP8266 && !VL_PLATFORM_STM32
// Defined in platform/<target>/*SystemInfo.cpp.
ISystemInfo& platformSystemInfo() {
  static ISystemInfo unknown;
  return unknown;
}
#endif

}  // namespace vl
