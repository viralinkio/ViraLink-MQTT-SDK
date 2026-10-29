#include "vl/pal/SystemInfo.h"

#include "vl/Config.h"

namespace vl {

#if !VL_PLATFORM_ESP32 && !VL_PLATFORM_ESP8266 && !VL_PLATFORM_STM32
// ESP32, ESP8266 and STM32 define this in platform/<target>/.
ISystemInfo& platformSystemInfo() {
  static ISystemInfo unknown;
  return unknown;
}
#endif

}  // namespace vl
