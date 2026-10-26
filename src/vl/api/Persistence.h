#pragma once

#include "vl/Config.h"

#if VL_ENABLE_PERSISTENCE

#include <stdint.h>

#include "vl/IApiModule.h"
#include "vl/core/Journal.h"
#include "vl/pal/OutboxStore.h"

namespace vl {

// Keeps the outbox across restarts and power cuts. Attaching it
// (client.use(persist), before begin()) restores what an earlier boot saved,
// ahead of anything queued since. After that, data records are saved every
// VL_OUTBOX_FLUSH_MS or VL_OUTBOX_FLUSH_RECORDS records and control records
// on the next loop(); a record already acknowledged by then is never written.
// After a restart, up to one save window of delivered records may be sent
// again (at least once). RPC replies and claims are not saved. Gateway
// records are saved with their sub-device's name: attach the gateway first,
// client.use(gw, persist), so restored records find their device. Runs on the
// client task.
class Persistence : public ApiModule {
 public:
  // store must outlive this module. It needs 3 or more segments and at
  // least twice the outbox's capacity.
  explicit Persistence(IOutboxStore& store) : journal_(store, scratch_, sizeof(scratch_)) {}

  // Outcome of the restore at attach: Ok, NotReady before attach, Rejected
  // for an unusable store or a store error (the client then runs RAM-only).
  Result status() const { return status_; }
  // Saves everything queued now, e.g. before deep sleep. Client task only.
  Result flush() { return journal_.flush(); }

  void attach(Client& client) override;
  void onDetach() override;
  void topics(TopicSink& sink) override { (void)sink; }
  bool handle(const Topic& topic, Bytes payload) override {
    (void)topic;
    (void)payload;
    return false;
  }
  void tick(uint64_t nowMs) override { journal_.tick(nowMs); }

 private:
  // The gateway's names, through the client.
  class Devices : public DeviceMap {
   public:
    Client* client = nullptr;
    const char* name(uint16_t dev) override;
    uint16_t index(const char* name) override;
  };

  Devices devices_;
  uint8_t scratch_[VL_OUTBOX_SCRATCH_SIZE];
  Journal journal_;
  Result status_ = Result::NotReady;
};

}  // namespace vl

#endif  // VL_ENABLE_PERSISTENCE
