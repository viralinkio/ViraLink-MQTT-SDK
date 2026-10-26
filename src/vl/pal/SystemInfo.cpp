#include "vl/pal/SystemInfo.h"

#include "vl/Config.h"

namespace vl {

#if !VL_PLATFORM_ESP32
// ESP32 defines this in platform/esp32/EspSystemInfo.cpp.
ISystemInfo& platformSystemInfo() {
  static ISystemInfo unknown;
  return unknown;
}
#endif

}  // namespace vl
