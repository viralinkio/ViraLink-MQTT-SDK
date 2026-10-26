#pragma once

#include "vl/Config.h"

#if VL_ENABLE_PERSISTENCE

#include <stddef.h>
#include <stdint.h>

#include "vl/Result.h"
#include "vl/core/Outbox.h"
#include "vl/pal/Lock.h"
#include "vl/pal/OutboxStore.h"

namespace vl {

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF).
uint16_t crc16(const uint8_t* p, size_t n, uint16_t crc = 0xFFFF);

// Write-ahead log of an outbox in an IOutboxStore. The ring stays the source
// of truth; the store holds record entries for what it queued and removal
// entries for saved records that left it, so a restart can rebuild it.
// Segments are filled in turn and reclaimed oldest first by copying their
// still-live records forward. Store calls run without the lock; the lock is
// taken only to copy a record out or update ring bookkeeping.
class Journal {
 public:
  // scratch holds one entry: a record of n payload bytes needs n + 34.
  Journal(IOutboxStore& store, uint8_t* scratch, size_t scratchCap)
      : store_(store), scratch_(scratch), scratchCap_(scratchCap) {}
  ~Journal() { end(); }
  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;

  // Replays the store into outbox (records queued before keep their order
  // after the restored ones) and starts saving it. Rejected if the store has
  // fewer than 3 segments or less than twice the ring's capacity.
  Result begin(Outbox& outbox, ILock& lock);
  // Stops saving and lets go of the outbox.
  void end();
  // Saves what is due. Call often from the client task.
  void tick(uint64_t nowMs);
  // Saves everything now. QueueFull if the store is full of live records,
  // Rejected after a store error.
  Result flush();
  // A store call failed: nothing more is saved until the next begin().
  bool failed() const { return failed_; }
  size_t segmentsUsed() const;

 private:
  struct Seg {
    uint32_t gen;
    bool used;
  };

  static void onRemoved(uint32_t seq, void* ctx);
  Result fail(Result r, const char* what);
  Result scan(uint8_t seg, bool apply, size_t& end, bool& clean, uint32_t& maxSeq);
  bool erasedFrom(uint8_t seg, size_t off);
  Result readHeader(uint8_t seg, uint32_t& gen);
  Result openNext();
  Result reclaim(uint8_t victim);
  Result writeHead(size_t len);
  Result syncHead();
  bool fillRecord(uint8_t seg, size_t& len);
  size_t freeCount() const;
  int oldest() const;

  IOutboxStore& store_;
  uint8_t* scratch_;
  size_t scratchCap_;
  Outbox* outbox_ = nullptr;
  ILock* lock_ = nullptr;

  Seg segs_[VL_OUTBOX_MAX_SEGMENTS] = {};
  uint8_t segCount_ = 0;
  size_t segSize_ = 0;
  uint32_t maxGen_ = 0;
  uint8_t head_ = 0;
  size_t headOff_ = 0;
  bool headOpen_ = false;
  bool headDirty_ = false;

  // Saved records that left the ring; guarded by the lock.
  uint32_t removed_[VL_OUTBOX_REMOVALS] = {};
  size_t removedCount_ = 0;
  uint32_t removalsLost_ = 0;

  bool dirty_ = false;
  uint64_t dirtySince_ = 0;
  uint64_t retryAt_ = 0;
  bool failed_ = false;
};

}  // namespace vl

#endif  // VL_ENABLE_PERSISTENCE
