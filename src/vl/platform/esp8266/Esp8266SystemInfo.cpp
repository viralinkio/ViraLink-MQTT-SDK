#include "vl/Config.h"

#if VL_PLATFORM_ESP8266

#include <Arduino.h>
#include <user_interface.h>

#include "vl/pal/SystemInfo.h"

namespace vl {

namespace {

// The non-OS SDK keeps no low-water mark, so vl_heap_min is left out.
class Esp8266SystemInfo : public ISystemInfo {
 public:
  bool heapFree(uint32_t& bytes) override {
    bytes = ESP.getFreeHeap();
    return true;
  }
  const char* resetReason() override {
    const rst_info* r = ESP.getResetInfoPtr();
    switch (r ? r->reason : 0xFF) {
      case REASON_DEFAULT_RST: return "power_on";
      case REASON_WDT_RST: return "wdt";
      case REASON_EXCEPTION_RST: return "panic";
      case REASON_SOFT_WDT_RST: return "task_wdt";
      case REASON_SOFT_RESTART: return "software";
      case REASON_DEEP_SLEEP_AWAKE: return "deep_sleep";
      case REASON_EXT_SYS_RST: return "external";
      default: return "unknown";
    }
  }
  const char* iface() override { return wifi_station_get_connect_status() == STATION_GOT_IP ? "wifi" : nullptr; }
  bool rssi(int16_t& dBm) override {
    if (wifi_station_get_connect_status() != STATION_GOT_IP) return false;
    sint8 v = wifi_station_get_rssi();
    if (v == 31) return false;  // the SDK's error value
    dBm = v;
    return true;
  }
};

}  // namespace

ISystemInfo& platformSystemInfo() {
  static Esp8266SystemInfo info;
  return info;
}

}  // namespace vl

#endif  // VL_PLATFORM_ESP8266
