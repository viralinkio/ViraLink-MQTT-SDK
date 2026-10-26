#pragma once

#include <stdint.h>

namespace vl {

// Chip and link vitals for the health attributes. Each value may be unknown
// on a platform; an unknown value is left out of the report. Used on the
// client task only.
class ISystemInfo {
 public:
  virtual ~ISystemInfo() {}
  // Free heap now, in bytes.
  virtual bool heapFree(uint32_t& bytes) {
    (void)bytes;
    return false;
  }
  // Lowest free heap since boot, in bytes.
  virtual bool heapMin(uint32_t& bytes) {
    (void)bytes;
    return false;
  }
  // Why the chip last reset: "power_on", "external", "software", "panic",
  // "int_wdt", "task_wdt", "wdt", "deep_sleep", "brownout", "sdio" or
  // "unknown". Null when the platform cannot tell.
  virtual const char* resetReason() { return nullptr; }
  // Interface carrying the session ("wifi", "eth", "gsm"; at most 8
  // characters), or null.
  virtual const char* iface() { return nullptr; }
  // Signal strength of that interface in dBm.
  virtual bool rssi(int16_t& dBm) {
    (void)dBm;
    return false;
  }
};

// The platform's source: ESP32 reports heap, reset reason and the WiFi station
// link; elsewhere every value is unknown for now.
ISystemInfo& platformSystemInfo();

}  // namespace vl
