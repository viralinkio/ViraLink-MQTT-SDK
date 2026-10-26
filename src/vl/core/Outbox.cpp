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
  uint8_t pad[3];
};

Outbox::Outbox(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap), maxPayload_(cap) {
  static_assert(sizeof(Hdr) == 40, "record header must stay 40 bytes (8-byte aligned)");
}

size_t Outbox::recordSize(size_t len) { return (sizeof(Hdr) + len + 7u) & ~static_cast<size_t>(7u); }

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

void Outbox::removeAt(size_t off) {
  size_t sz = next(off) - off;
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

Result Outbox::push(const RecordMeta& meta, const uint8_t* payload, size_t len) {
  size_t need = recordSize(len);
  if (len > maxPayload_ || len > 0xFFFF || need > cap_ - controlReserve()) return Result::TooLarge;

  bool data = meta.cls == RecordClass::Data;
  size_t limit = data ? cap_ - controlReserve() : cap_;
  while (bytesUsed() + need > limit) {
    if (!evictOneData()) {
      if (data) ++dropped_;
      return Result::QueueFull;
    }
  }

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

  if (pos == head_ && head_ >= need && head_ != tail_) {
    head_ -= need;
    pos = head_;
  } else {
    if (cap_ - tail_ < need) {
      size_t rel = pos - head_;
      compact();
      pos = head_ + rel;
    }
    memmove(buf_ + pos + need, buf_ + pos, tail_ - pos);
    tail_ += need;
  }

  Hdr h;
  memset(&h, 0, sizeof(h));
  h.time = meta.time;
  h.seq = nextSeq_++;
  h.arg = meta.arg;
  h.arg2 = meta.arg2;
  h.len = static_cast<uint16_t>(len);
  h.dev = meta.dev;
  h.bootId = meta.bootId;
  h.topic = static_cast<uint8_t>(meta.topic);
  h.cls = static_cast<uint8_t>(meta.cls);
  h.timeKind = static_cast<uint8_t>(meta.timeKind);
  writeHdr(pos, h);
  if (len) memcpy(buf_ + pos + sizeof(Hdr), payload, len);
  return Result::Queued;
}

bool Outbox::beginSend(RecordView& out) {
  for (size_t off = head_; off < tail_; off = next(off)) {
    Hdr h;
    readHdr(off, h);
    if (h.flags & (kInFlight | kSending)) continue;
    h.flags |= kSending;
    writeHdr(off, h);
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

size_t Outbox::inFlight() const {
  size_t n = 0;
  for (size_t off = head_; off < tail_; off = next(off)) {
    Hdr h;
    readHdr(off, h);
    if (h.flags & kInFlight) ++n;
  }
  return n;
}

}  // namespace vl
