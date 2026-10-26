#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vl/Result.h"
#include "vl/core/Topics.h"

namespace vl {

// Classes decide only what may be dropped and where a record is inserted.
enum class RecordClass : uint8_t {
  Data,      // telemetry, attributes: dropped oldest-first when the ring is full
  Control,   // fw_state, claims, gateway connect/disconnect, provisioning: never dropped
  Priority,  // RPC replies: never dropped and inserted ahead of everything unsent
};

enum class TimeKind : uint8_t {
  None,    // send without "ts"
  Uptime,  // "time" is uptime ms of boot "bootId"; converted to UTC at publish
  Utc,     // "time" is already UTC ms
};

const uint16_t kNoDevice = 0xFFFF;

struct RecordMeta {
  OutTopic topic = OutTopic::Telemetry;
  RecordClass cls = RecordClass::Data;
  TimeKind timeKind = TimeKind::None;
  uint16_t dev = kNoDevice;  // gateway sub-device index
  uint16_t bootId = 0;
  uint32_t arg = 0;
  uint32_t arg2 = 0;
  uint64_t time = 0;
};

struct RecordView {
  RecordMeta meta;
  uint32_t seq = 0;
  uint8_t strikes = 0;
  const uint8_t* payload = nullptr;  // valid until the next outbox call
  size_t len = 0;
};

// FIFO of publish records in a fixed byte buffer. Records leave only when the
// broker acknowledges them (ack), or as counted drops/dead letters. Not
// thread-safe: the client serialises access with its lock.
//
// The live region [head, tail) is kept contiguous; it is compacted to the
// start of the buffer when the tail runs out of room, so every payload is one
// contiguous span.
class Outbox {
 public:
  static const uint8_t kMaxStrikes = 3;
  static const uint32_t kPoisonWindowMs = 5000;

  Outbox(uint8_t* buf, size_t cap);

  // Largest payload accepted at enqueue (the transport's max publish minus
  // envelope overhead). Larger payloads return TooLarge.
  void setMaxPayload(size_t n) { maxPayload_ = n; }
  size_t maxPayload() const { return maxPayload_; }

  Result push(const RecordMeta& meta, const uint8_t* payload, size_t len);

  // Oldest record that is neither in flight nor being sent; marks it Sending.
  bool beginSend(RecordView& out);
  // Publish succeeded: record now waits for its PUBACK.
  void markSent(uint32_t seq, uint16_t packetId, uint64_t nowMs);
  // Publish failed: the record becomes sendable again.
  void markUnsent(uint32_t seq);
  // Removes a record that can never be published; counted as a dead letter.
  void discard(uint32_t seq);
  // PUBACK: frees the record. False if no record waits for this id.
  bool ack(uint16_t packetId);
  // Session dropped. Applies a poison strike to the oldest in-flight record if
  // it was sent within kPoisonWindowMs, then makes every record sendable again.
  void onSessionClosed(uint64_t nowMs);

  // Rewrites matching records' time from uptime to UTC once the clock syncs.
  // Returns the number of records stamped.
  size_t stampUtc(uint16_t bootId, uint64_t uptimeNow, uint64_t utcNow);

  size_t records() const;
  // The index-th record from the head, for inspection. O(index).
  bool peek(size_t index, RecordView& out) const;
  size_t inFlight() const;
  size_t bytesUsed() const { return tail_ - head_; }
  size_t capacity() const { return cap_; }
  size_t controlReserve() const { return cap_ / 10; }
  uint32_t dropped() const { return dropped_; }
  uint32_t deadLetters() const { return deadLetters_; }

  // ---- Persistence (vl::Journal) ----
  // Each record remembers the store segment holding its saved copy, or one of:
  static const uint8_t kUnsaved = 0xFF;   // not saved yet
  static const uint8_t kNotSaved = 0xFE;  // never saved (RPC replies, records too large to save)
  typedef void (*RemoveHook)(uint32_t seq, void* ctx);
  // Called whenever a saved record leaves the ring (PUBACK, eviction, dead
  // letter), from inside the outbox call, so under the caller's lock.
  void setRemoveHook(RemoveHook fn, void* ctx) {
    hook_ = fn;
    hookCtx_ = ctx;
  }
  // Oldest record not saved yet. The payload view is valid until the next call.
  bool nextUnsaved(RecordView& out) const;
  // A record whose saved copy is in segment seg.
  bool nextInSegment(uint8_t seg, RecordView& out) const;
  // Records where seq's saved copy now lives (a segment, or kNotSaved).
  void setSaved(uint32_t seq, uint8_t seg);
  size_t unsaved() const { return unsaved_; }
  size_t unsavedControl() const { return unsavedControl_; }
  // Puts a record read back from segment seg into the ring with its own seq:
  // in seq order among restored records, ahead of everything queued this
  // boot. A seq already restored only moves to seg. Data evicts older data
  // like push(). Ok, QueueFull or TooLarge.
  Result restore(const RecordView& rec, uint8_t seg);
  // Removes a record without counting it or calling the hook.
  void forget(uint32_t seq);
  // Ends a restore: records queued before it are renumbered after maxSeq, the
  // highest seq in the store, and numbering continues from there.
  void finishRestore(uint32_t maxSeq);

 private:
  struct Hdr;
  static size_t recordSize(size_t len);
  static bool saved(const Hdr& h);
  void readHdr(size_t off, Hdr& h) const;
  void writeHdr(size_t off, const Hdr& h);
  size_t next(size_t off) const;
  void removeAt(size_t off, bool notify = true);
  void compact();
  bool evictOneData();
  Result makeRoom(size_t need, bool data);
  size_t insertAt(size_t pos, size_t need);
  void writeRecord(size_t pos, const RecordMeta& meta, uint32_t seq, uint8_t seg, const uint8_t* payload, size_t len);
  void toView(size_t off, const Hdr& h, RecordView& out) const;
  size_t findSeq(uint32_t seq, Hdr& h) const;

  uint8_t* buf_;
  size_t cap_;
  size_t head_ = 0;
  size_t tail_ = 0;
  size_t maxPayload_;
  uint32_t nextSeq_ = 1;
  uint32_t dropped_ = 0;
  uint32_t deadLetters_ = 0;
  size_t unsaved_ = 0;
  size_t unsavedControl_ = 0;
  RemoveHook hook_ = nullptr;
  void* hookCtx_ = nullptr;
};

// Outbox with its own storage. N is rounded down to a multiple of 8.
template <size_t N>
class RingOutbox : public Outbox {
 public:
  RingOutbox() : Outbox(storage_, N & ~static_cast<size_t>(7)) {}

 private:
  alignas(8) uint8_t storage_[N];
};

}  // namespace vl
