#include "vl/api/Health.h"

#if VL_ENABLE_HEALTH

#include "vl/Client.h"
#include "vl/Log.h"
#include "vl/core/JsonWriter.h"

namespace vl {

namespace {

// Fixed for a boot; the server keeps the value from the connect report.
const uint16_t kStaticKeys = kHealthResetReason | kHealthSdk;

}  // namespace

void Health::onOnline() {
  if (!client_) return;
  uint64_t now = client_->uptimeMs();
  next_ = now + intervalMs_;
  report(keys_, now);
}

void Health::tick(uint64_t nowMs) {
  if (!client_ || intervalMs_ == 0 || nowMs < next_ || !client_->online()) return;
  next_ = nowMs + intervalMs_;
  report(keys_ & ~kStaticKeys, nowMs);
}

void Health::report(uint16_t keys, uint64_t nowMs) {
  if (keys == 0) return;
  HealthSample s;
  s.keys = keys;
  s.uptimeS = static_cast<uint32_t>(nowMs / 1000);
  if (!source_->heapFree(s.heapFree)) s.keys &= ~kHealthHeapFree;
  if (!source_->heapMin(s.heapMin)) s.keys &= ~kHealthHeapMin;
  if (!source_->rssi(s.rssi)) s.keys &= ~kHealthRssi;
  s.resetReason = source_->resetReason();
  s.iface = source_->iface();
  Stats st = client_->stats();
  s.queueDepth = static_cast<uint32_t>(st.queuedRecords);
  s.dropped = st.dropped;
  s.deadLetters = st.deadLetters;
  s.sdk = VL_SDK_VERSION;

  char buf[VL_HEALTH_SIZE];
  JsonWriter w(buf, sizeof(buf));
  codec::health(w, s, v1Keys_);
  if (w.overflowed()) {
    VL_LOGW("health: report larger than VL_HEALTH_SIZE");
    return;
  }
  RecordMeta m;
  m.topic = OutTopic::Attributes;
  Result r = client_->enqueue(m, reinterpret_cast<const uint8_t*>(buf), w.size());
  if (!ok(r)) VL_LOGW("health: report not queued: %s", toString(r));
}

}  // namespace vl

#endif  // VL_ENABLE_HEALTH
