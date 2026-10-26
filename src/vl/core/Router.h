#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vl/Config.h"
#include "vl/IApiModule.h"
#include "vl/Result.h"

namespace vl {

// Holds the attached modules, the subscription registry derived from them,
// and routes incoming messages by topic kind. Binary topics (OTA chunks) are
// passed through untouched: no JSON parse, no copy.
class Router {
 public:
  Result add(IApiModule& m);
  void remove(IApiModule& m);

  size_t moduleCount() const { return count_; }
  IApiModule& module(size_t i) { return *modules_[i]; }
  // Cached at add(): the module itself may be gone by the time this is read.
  bool detachesItself(size_t i) const { return selfDetach_[i]; }

  // Subscription filters, in a stable order.
  size_t filterCount() const;
  const char* filter(size_t i) const;
  bool subscribed(InTopic kind) const { return (allKinds_ & kindBit(kind)) != 0; }

  // Returns true if some module consumed the message. AttrUpdate reaches
  // every subscribed module; other kinds stop at the first that consumes.
  bool dispatch(const char* topic, Bytes payload);

 private:
  static uint32_t kindBit(InTopic k) { return 1u << static_cast<uint8_t>(k); }

  class Sink : public TopicSink {
   public:
    uint32_t kinds = 0;
    void add(InTopic kind) override { kinds |= 1u << static_cast<uint8_t>(kind); }
  };

  IApiModule* modules_[VL_MAX_MODULES] = {};
  uint32_t kinds_[VL_MAX_MODULES] = {};
  bool selfDetach_[VL_MAX_MODULES] = {};
  uint32_t allKinds_ = 0;
  size_t count_ = 0;
};

}  // namespace vl
