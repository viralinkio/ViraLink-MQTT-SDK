#include "vl/core/Outbox.h"

#include <string.h>

#include "vl/Log.h"

namespace vl {

namespace {
const uint8_t kInFlight = 1;
const uint8_t kSending = 2;
const size_t kNpos = static_cast<size_t>(-1);
}  // namespace

struct Outbox::Hdr {
  uint64_t time;
  uint32_t seq;
  uint32_t arg;
  uint32_t arg2;
  uint32_t sentAt;  // low 32 bits of uptime ms; compared with wrap-safe subtraction
  uint16_t len;
  uint16_t dev;
  uint16_t bootId;
  uint16_t packetId;
  uint8_t topic;
  uint8_t cls;
  uint8_t timeKind;
  uint8_t flags;
  uint8_t strikes;
  uint8_t seg;  // store segment of the saved copy, kUnsaved or kNotSaved
  uint8_t pad[2];
};

Outbox::Outbox(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap), maxPayload_(cap) {
  static_assert(sizeof(Hdr) == 40, "record header must stay 40 bytes (8-byte aligned)");
}

size_t Outbox::recordSize(size_t len) { return (sizeof(Hdr) + len + 7u) & ~static_cast<size_t>(7u); }

bool Outbox::saved(const Hdr& h) { return h.seg != kUnsaved && h.seg != kNotSaved; }

void Outbox::readHdr(size_t off, Hdr& h) const { memcpy(&h, buf_ + off, sizeof(Hdr)); }

void Outbox::writeHdr(size_t off, const Hdr& h) { memcpy(buf_ + off, &h, sizeof(Hdr)); }

size_t Outbox::next(size_t off) const {
  Hdr h;
  readHdr(off, h);
  return off + recordSize(h.len);
}

void Outbox::compact() {
  if (head_ == 0) return;
  memmove(buf_, buf_ + head_, tail_ - head_);
  tail_ -= head_;
  head_ = 0;
}

void Outbox::removeAt(size_t off, bool notify) {
  Hdr h;
  readHdr(off, h);
  if (h.seg == kUnsaved) {
    --unsaved_;
    if (h.cls == static_cast<uint8_t>(RecordClass::Control)) --unsavedControl_;
  } else if (notify && hook_ && saved(h)) {
    hook_(h.seq, hookCtx_);
  }
  size_t sz = recordSize(h.len);
  if (off == head_) {
    head_ += sz;
  } else {
    memmove(buf_ + off, buf_ + off + sz, tail_ - off - sz);
    tail_ -= sz;
  }
  if (head_ == tail_) head_ = tail_ = 0;
}

bool Outbox::evictOneData() {
  for (size_t off = head_; off < tail_; off = next(off)) {
    Hdr h;
    readHdr(off, h);
    if (h.cls == static_cast<uint8_t>(RecordClass::Data) && !(h.flags & (kInFlight | kSending))) {
      removeAt(off);
      ++dropped_;
      return true;
    }
  }
  return false;
}

Result Outbox::makeRoom(size_t need, bool data) {
  size_t limit = data ? cap_ - controlReserve() : cap_;
  while (bytesUsed() + need > limit) {
    if (!evictOneData()) {
      if (data) ++dropped_;
      return Result::QueueFull;
    }
  }
  return Result::Ok;
}

// Opens need bytes at pos, which must be a record boundary; returns where the
// gap ended up after any compaction.
size_t Outbox::insertAt(size_t pos, size_t need) {
  if (pos == head_ && head_ >= need && head_ != tail_) {
    head_ -= need;
    return head_;
  }
  if (cap_ - tail_ < need) {
    size_t rel = pos - head_;
    compact();
    pos = head_ + rel;
  }
  memmove(buf_ + pos + need, buf_ + pos, tail_ - pos);
  tail_ += need;
  return pos;
}

void Outbox::writeRecord(size_t pos, const RecordMeta& meta, uint32_t seq, uint8_t seg, const uint8_t* payload,
                         size_t len) {
  Hdr h;
  memset(&h, 0, sizeof(h));
  h.time = meta.time;
  h.seq = seq;
  h.arg = meta.arg;
  h.arg2 = meta.arg2;
  h.len = static_cast<uint16_t>(len);
  h.dev = meta.dev;
  h.bootId = meta.bootId;
  h.topic = static_cast<uint8_t>(meta.topic);
  h.cls = static_cast<uint8_t>(meta.cls);
  h.timeKind = static_cast<uint8_t>(meta.timeKind);
  h.seg = seg;
  writeHdr(pos, h);
  if (len) memcpy(buf_ + pos + sizeof(Hdr), payload, len);
  if (seg == kUnsaved) {
    ++unsaved_;
    if (meta.cls == RecordClass::Control) ++unsavedControl_;
  }
}

Result Outbox::push(const RecordMeta& meta, const uint8_t* payload, size_t len) {
  size_t need = recordSize(len);
  if (len > maxPayload_ || len > 0xFFFF || need > cap_ - controlReserve()) return Result::TooLarge;
  Result r = makeRoom(need, meta.cls == RecordClass::Data);
  if (r != Result::Ok) return r;

  // Priority records go after any earlier priority records but ahead of
  // everything else, so replies stay FIFO among themselves.
  size_t pos = tail_;
  if (meta.cls == RecordClass::Priority) {
    pos = head_;
    while (pos < tail_) {
      Hdr h;
      readHdr(pos, h);
      if (h.cls != static_cast<uint8_t>(RecordClass::Priority)) break;
      pos = next(pos);
    }
  }
  pos = insertAt(pos, need);
  bool save = meta.cls != RecordClass::Priority && !meta.transient;
  writeRecord(pos, meta, nextSeq_++, save ? kUnsaved : kNotSaved, payload, len);
  return Result::Queued;
}

void Outbox::toView(size_t off, const Hdr& h, RecordView& out) const {
  out.meta.topic = static_cast<OutTopic>(h.topic);
  out.meta.cls = static_cast<RecordClass>(h.cls);
  out.meta.timeKind = static_cast<TimeKind>(h.timeKind);
  out.meta.dev = h.dev;
  out.meta.bootId = h.bootId;
  out.meta.arg = h.arg;
  out.meta.arg2 = h.arg2;
  out.meta.time = h.time;
  out.seq = h.seq;
  out.strikes = h.strikes;
  out.payload = buf_ + off + sizeof(Hdr);
  out.len = h.len;
}

bool Outbox::beginSend(RecordView& out) {
  for (size_t off = head_; off < tail_; off = next(off)) {
    Hdr h;
    readHdr(off, h);
    if (h.flags & (kInFlight | kSending)) continue;
    h.flags |= kSending;
    writeHdr(off, h);
    toView(off, h, out);
    return true;
  }
  return false;
}

size_t Outbox::findSeq(uint32_t seq, Hdr& h) const {
  for (size_t off = head_; off < tail_; off = next(off)) {
    readHdr(off, h);
    if (h.seq == seq) return off;
  }
  return kNpos;
}

void Outbox::markSent(uint32_t seq, uint16_t packetId, uint64_t nowMs) {
  Hdr h;
  size_t off = findSeq(seq, h);
  if (off == kNpos) return;
  h.flags = static_cast<uint8_t>((h.flags & ~kSending) | kInFlight);
  h.packetId = packetId;
  h.sentAt = static_cast<uint32_t>(nowMs);
  writeHdr(off, h);
}

void Outbox::markUnsent(uint32_t seq) {
  Hdr h;
  size_t off = findSeq(seq, h);
  if (off == kNpos) return;
  h.flags = static_cast<uint8_t>(h.flags & ~(kSending | kInFlight));
  writeHdr(off, h);
}

void Outbox::discard(uint32_t seq) {
  Hdr h;
  size_t off = findSeq(seq, h);
  if (off == kNpos) return;
  removeAt(off);
  ++deadLetters_;
}

bool Outbox::ack(uint16_t packetId) {
  for (size_t off = head_; off < tail_; off = next(off)) {
    Hdr h;
    readHdr(off, h);
    if ((h.flags & kInFlight) && h.packetId == packetId) {
      removeAt(off);
      return true;
    }
  }
  return false;
}

void Outbox::onSessionClosed(uint64_t nowMs) {
  size_t oldest = kNpos;
  uint32_t oldestSeq = 0;
  for (size_t off = head_; off < tail_; off = next(off)) {
    Hdr h;
    readHdr(off, h);
    if ((h.flags & kInFlight) && (oldest == kNpos || static_cast<int32_t>(h.seq - oldestSeq) < 0)) {
      oldest = off;
      oldestSeq = h.seq;
    }
  }
  if (oldest != kNpos) {
    Hdr h;
    readHdr(oldest, h);
    if (static_cast<uint32_t>(nowMs) - h.sentAt < kPoisonWindowMs) {
      ++h.strikes;
      writeHdr(oldest, h);
      if (h.strikes >= kMaxStrikes) {
        VL_LOGE("outbox: record seq=%lu topic=%u dead-lettered after %u strikes",
                static_cast<unsigned long>(h.seq), h.topic, h.strikes);
        removeAt(oldest);
        ++deadLetters_;
      }
    }
  }
  for (size_t off = head_; off < tail_; off = next(off)) {
    Hdr h;
    readHdr(off, h);
    if (h.flags & (kInFlight | kSending)) {
      h.flags = static_cast<uint8_t>(h.flags & ~(kInFlight | kSending));
      writeHdr(off, h);
    }
  }
}

size_t Outbox::stampUtc(uint16_t bootId, uint64_t uptimeNow, uint64_t utcNow) {
  size_t n = 0;
  for (size_t off = head_; off < tail_; off = next(off)) {
    Hdr h;
    readHdr(off, h);
    if (h.timeKind == static_cast<uint8_t>(TimeKind::Uptime) && h.bootId == bootId && h.time <= uptimeNow) {
      h.time = utcNow - (uptimeNow - h.time);
      h.timeKind = static_cast<uint8_t>(TimeKind::Utc);
      writeHdr(off, h);
      ++n;
    }
  }
  return n;
}

size_t Outbox::records() const {
  size_t n = 0;
  for (size_t off = head_; off < tail_; off = next(off)) ++n;
  return n;
}

bool Outbox::peek(size_t index, RecordView& out) const {
  for (size_t off = head_; off < tail_; off = next(off)) {
    if (index-- == 0) {
      Hdr h;
      readHdr(off, h);
      toView(off, h, out);
      return true;
    }
  }
  return false;
}

size_t Outbox::inFlight() const {
  size_t n = 0;
  for (size_t off = head_; off < tail_; off = next(off)) {
    Hdr h;
    readHdr(off, h);
    if (h.flags & kInFlight) ++n;
  }
  return n;
}

// ---- Persistence ----

bool Outbox::nextUnsaved(RecordView& out) const {
  for (size_t off = head_; off < tail_; off = next(off)) {
    Hdr h;
    readHdr(off, h);
    if (h.seg == kUnsaved) {
      toView(off, h, out);
      return true;
    }
  }
  return false;
}

bool Outbox::nextInSegment(uint8_t seg, RecordView& out) const {
  for (size_t off = head_; off < tail_; off = next(off)) {
    Hdr h;
    readHdr(off, h);
    if (h.seg == seg) {
      toView(off, h, out);
      return true;
    }
  }
  return false;
}

void Outbox::setSaved(uint32_t seq, uint8_t seg) {
  Hdr h;
  size_t off = findSeq(seq, h);
  if (off == kNpos) return;
  if (h.seg == kUnsaved && seg != kUnsaved) {
    --unsaved_;
    if (h.cls == static_cast<uint8_t>(RecordClass::Control)) --unsavedControl_;
  }
  h.seg = seg;
  writeHdr(off, h);
}

Result Outbox::restore(const RecordView& rec, uint8_t seg) {
  size_t need = recordSize(rec.len);
  if (rec.len > maxPayload_ || rec.len > 0xFFFF || need > cap_ - controlReserve()) return Result::TooLarge;
  for (size_t off = head_; off < tail_; off = next(off)) {
    Hdr h;
    readHdr(off, h);
    if (saved(h) && h.seq == rec.seq) {
      h.seg = seg;
      writeHdr(off, h);
      return Result::Ok;
    }
  }
  Result r = makeRoom(need, rec.meta.cls == RecordClass::Data);
  if (r != Result::Ok) return r;
  size_t pos = head_;
  while (pos < tail_) {
    Hdr h;
    readHdr(pos, h);
    if (!saved(h) || h.seq > rec.seq) break;
    pos = next(pos);
  }
  pos = insertAt(pos, need);
  writeRecord(pos, rec.meta, rec.seq, seg, rec.payload, rec.len);
  return Result::Ok;
}

void Outbox::forget(uint32_t seq) {
  // Only saved records: until finishRestore() a record queued this boot may
  // carry the same seq.
  for (size_t off = head_; off < tail_; off = next(off)) {
    Hdr h;
    readHdr(off, h);
    if (saved(h) && h.seq == seq) {
      removeAt(off, false);
      return;
    }
  }
}

void Outbox::finishRestore(uint32_t maxSeq) {
  uint32_t seq = maxSeq + 1;
  for (size_t off = head_; off < tail_; off = next(off)) {
    Hdr h;
    readHdr(off, h);
    if (!saved(h)) {
      h.seq = seq++;
      writeHdr(off, h);
    }
  }
  nextSeq_ = seq;
}

}  // namespace vl
