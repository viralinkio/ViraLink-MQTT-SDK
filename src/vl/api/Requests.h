#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vl/Client.h"

namespace vl {

// Outstanding requests a module started (client RPC, attribute requests),
// each holding the user callback to fire exactly once. start(), open(),
// submit() and abort() are thread-safe; finish() runs on the client task.
template <class Cb, size_t N>
class RequestSlots {
 public:
  // For payloads that embed the request id: claims a slot and a correlator
  // entry (owned by the module, so it is dropped if the module goes away) and
  // returns the id, or 0 when either is full. done must call finish(). Follow
  // with submit() or abort().
  uint32_t open(Client& c, const IApiModule* owner, uint32_t timeoutMs, Correlator::Callback done, Cb cb, void* ctx,
                void*& handle) {
    Slot* slot = nullptr;
    {
      LockGuard g(c.lock());
      for (size_t i = 0; i < N && !slot; ++i) {
        if (slots_[i].used) continue;
        slot = &slots_[i];
        slot->client = &c;
        slot->cb = cb;
        slot->ctx = ctx;
        slot->used = true;
      }
    }
    if (!slot) return 0;
    uint32_t id = c.addPending(timeoutMs, done, slot, owner);
    if (id == 0) {
      release(slot);
      return 0;
    }
    handle = slot;
    return id;
  }

  // Queues an opened request; on failure it is dropped without firing.
  Result submit(Client& c, uint32_t id, void* handle, const RecordMeta& meta, const char* payload, size_t len) {
    Result r = c.enqueue(meta, reinterpret_cast<const uint8_t*>(payload), len);
    if (!ok(r)) abort(c, id, handle);
    return r;
  }

  // Drops an opened request without firing it.
  void abort(Client& c, uint32_t id, void* handle) {
    // If the cancel loses a race with the timeout, done frees the slot.
    if (c.cancelPending(id)) release(static_cast<Slot*>(handle));
  }

  // open() and submit() with meta.arg = request id. NoMemory when no slot or
  // correlator entry is free.
  Result start(Client& c, const IApiModule* owner, RecordMeta meta, const char* payload, size_t len,
               uint32_t timeoutMs, Correlator::Callback done, Cb cb, void* ctx) {
    void* handle = nullptr;
    uint32_t id = open(c, owner, timeoutMs, done, cb, ctx, handle);
    if (id == 0) return Result::NoMemory;
    meta.arg = id;
    return submit(c, id, handle, meta, payload, len);
  }

  // Called from done with its ctx: returns the user callback, frees the slot.
  static void finish(void* slotCtx, Cb& cb, void*& ctx) {
    Slot* slot = static_cast<Slot*>(slotCtx);
    cb = slot->cb;
    ctx = slot->ctx;
    release(slot);
  }

 private:
  struct Slot {
    Client* client = nullptr;
    Cb cb = nullptr;
    void* ctx = nullptr;
    bool used = false;
  };

  static void release(Slot* slot) {
    LockGuard g(slot->client->lock());
    *slot = Slot();
  }

  Slot slots_[N];
};

}  // namespace vl
