#include "vl/core/Router.h"

#include "vl/Log.h"

namespace vl {

Result Router::add(IApiModule& m) {
  for (size_t i = 0; i < count_; ++i) {
    if (modules_[i] == &m) return Result::Ok;
  }
  if (count_ >= VL_MAX_MODULES) return Result::NoMemory;
  Sink sink;
  m.topics(sink);
  sink.kinds &= ~kindBit(InTopic::Unknown);
  modules_[count_] = &m;
  kinds_[count_] = sink.kinds;
  selfDetach_[count_] = m.detachesItself();
  allKinds_ |= sink.kinds;
  ++count_;
  return Result::Ok;
}

void Router::remove(IApiModule& m) {
  if (solo_ == &m) setSolo(nullptr);
  size_t i = 0;
  while (i < count_ && modules_[i] != &m) ++i;
  if (i == count_) return;
  for (; i + 1 < count_; ++i) {
    modules_[i] = modules_[i + 1];
    kinds_[i] = kinds_[i + 1];
    selfDetach_[i] = selfDetach_[i + 1];
  }
  --count_;
  modules_[count_] = nullptr;
  kinds_[count_] = 0;
  selfDetach_[count_] = false;
  allKinds_ = 0;
  for (i = 0; i < count_; ++i) allKinds_ |= kinds_[i];
}

void Router::setSolo(const IApiModule* m) {
  solo_ = nullptr;
  soloKinds_ = 0;
  for (size_t i = 0; m && i < count_; ++i) {
    if (modules_[i] != m) continue;
    solo_ = m;
    soloKinds_ = kinds_[i];
  }
}

size_t Router::filterCount() const {
  size_t n = 0;
  for (uint8_t k = 1; k < static_cast<uint8_t>(InTopic::Count); ++k) {
    if (activeKinds() & (1u << k)) ++n;
  }
  return n;
}

InTopic Router::filterKind(size_t i) const {
  for (uint8_t k = 1; k < static_cast<uint8_t>(InTopic::Count); ++k) {
    if (!(activeKinds() & (1u << k))) continue;
    if (i == 0) return static_cast<InTopic>(k);
    --i;
  }
  return InTopic::Unknown;
}

const char* Router::filter(size_t i) const {
  InTopic k = filterKind(i);
  return k == InTopic::Unknown ? nullptr : filterFor(k);
}

bool Router::dispatch(const char* topic, Bytes payload) {
  Topic t = parseTopic(topic);
  if (t.kind == InTopic::Unknown) {
    VL_LOGW("router: unexpected topic %s", topic);
    return false;
  }
  // A shared-attribute update goes to every subscriber (attributes, OTA);
  // anything else stops at the first module that consumes it.
  bool all = t.kind == InTopic::AttrUpdate;
  bool consumed = false;
  for (size_t i = 0; i < count_; ++i) {
    if (solo_ && modules_[i] != solo_) continue;
    if (!(kinds_[i] & kindBit(t.kind)) || !modules_[i]->handle(t, payload)) continue;
    consumed = true;
    if (!all) return true;
  }
  if (!consumed) VL_LOGD("router: no module consumed %s", topic);
  return consumed;
}

}  // namespace vl
