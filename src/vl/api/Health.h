#pragma once

#include "vl/Config.h"

#if VL_ENABLE_HEALTH

#include <stdint.h>

#include "vl/IApiModule.h"
#include "vl/core/Codec.h"
#include "vl/pal/SystemInfo.h"

namespace vl {

// Reports the device's vitals as client attributes (vl_uptime, vl_heap_free,
// vl_heap_min, vl_reset_reason, vl_rssi, vl_iface, vl_queue_depth,
// vl_dropped, vl_dead_letter, vl_sdk). Every entry into Online sends all
// enabled keys; each interval after that sends the ones that change during a
// boot (all but vl_sdk and vl_reset_reason). Nothing is queued while offline:
// a stale snapshot is not worth ring space. Setters run before begin() or on
// the client task.
class Health : public ApiModule {
 public:
  // intervalMs 0: report only on connect.
  explicit Health(uint32_t intervalMs = VL_HEALTH_INTERVAL_MS)
      : source_(&platformSystemInfo()), intervalMs_(intervalMs) {}

  void setInterval(uint32_t ms) { intervalMs_ = ms; }
  // kHealth* bits to report; default kHealthAll.
  void setKeys(uint16_t keys) { keys_ = keys; }
  // The 1.0.2 names for uptime, free heap and SDK version (VL_COMPAT_V1_KEYS
  // sets the default).
  void useV1Keys(bool on) { v1Keys_ = on; }
  // Replaces the platform's source; must outlive this module.
  void setSource(ISystemInfo& s) { source_ = &s; }

  void topics(TopicSink& sink) override { (void)sink; }
  bool handle(const Topic& topic, Bytes payload) override {
    (void)topic;
    (void)payload;
    return false;
  }
  void onOnline() override;
  void tick(uint64_t nowMs) override;

 private:
  void report(uint16_t keys, uint64_t nowMs);

  ISystemInfo* source_;
  uint32_t intervalMs_;
  uint16_t keys_ = kHealthAll;
  bool v1Keys_ = VL_COMPAT_V1_KEYS;
  uint64_t next_ = 0;
};

}  // namespace vl

#endif  // VL_ENABLE_HEALTH
