#include "vl/pal/SystemInfo.h"

#include "vl/Config.h"

namespace vl {

#if !VL_PLATFORM_ESP32 && !VL_PLATFORM_ESP8266
// Defined in platform/esp32/EspSystemInfo.cpp and platform/esp8266/Esp8266SystemInfo.cpp.
ISystemInfo& platformSystemInfo() {
  static ISystemInfo unknown;
  return unknown;
}
#endif

}  // namespace vl
