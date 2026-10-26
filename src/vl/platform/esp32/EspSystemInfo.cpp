#include "vl/Config.h"

#if VL_PLATFORM_ESP32

#include "esp_system.h"
#include "esp_wifi.h"
#include "vl/pal/SystemInfo.h"

namespace vl {

namespace {

// Reports the WiFi station link while it is associated. Ethernet and cellular
// arrive with the network manager.
class EspSystemInfo : public ISystemInfo {
 public:
  bool heapFree(uint32_t& bytes) override {
    bytes = esp_get_free_heap_size();
    return true;
  }
  bool heapMin(uint32_t& bytes) override {
    bytes = esp_get_minimum_free_heap_size();
    return true;
  }
  const char* resetReason() override {
    switch (esp_reset_reason()) {
      case ESP_RST_POWERON: return "power_on";
      case ESP_RST_EXT: return "external";
      case ESP_RST_SW: return "software";
      case ESP_RST_PANIC: return "panic";
      case ESP_RST_INT_WDT: return "int_wdt";
      case ESP_RST_TASK_WDT: return "task_wdt";
      case ESP_RST_WDT: return "wdt";
      case ESP_RST_DEEPSLEEP: return "deep_sleep";
      case ESP_RST_BROWNOUT: return "brownout";
      case ESP_RST_SDIO: return "sdio";
      default: return "unknown";
    }
  }
  const char* iface() override {
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? "wifi" : nullptr;
  }
  bool rssi(int16_t& dBm) override {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    dBm = ap.rssi;
    return true;
  }
};

}  // namespace

ISystemInfo& platformSystemInfo() {
  static EspSystemInfo info;
  return info;
}

}  // namespace vl

#endif  // VL_PLATFORM_ESP32
