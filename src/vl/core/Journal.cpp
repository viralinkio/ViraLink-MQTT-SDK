#include "vl/core/Journal.h"

#if VL_ENABLE_PERSISTENCE

#include <string.h>

#include "vl/Log.h"

namespace vl {

namespace {

// Entry: type u8 | 0 u8 | len u16 | body[len] | crc16 u16, host byte order.
const uint8_t kTypeHeader = 1;    // magic u32, generation u32
const uint8_t kTypeRecord = 2;    // see encodeRecord()
const uint8_t kTypeRemovals = 3;  // seq u32 x n
const uint8_t kErased = 0xFF;
const uint32_t kMagic = 0x314A4C56;  // "VLJ1"
const size_t kEntryHead = 4;
const size_t kCrcSize = 2;
const size_t kHeaderEntry = kEntryHead + 8 + kCrcSize;
const size_t kRecordMeta = 28;
const size_t kRecordOverhead = kEntryHead + kRecordMeta + kCrcSize;
const size_t kRemovalsEntry = kEntryHead + 4 * VL_OUTBOX_REMOVALS + kCrcSize;
const uint8_t kNoSeg = 0xFF;

template <class T>
void put(uint8_t* p, T v) {
  memcpy(p, &v, sizeof(v));
}

template <class T>
T get(const uint8_t* p) {
  T v;
  memcpy(&v, p, sizeof(v));
  return v;
}

// Completes the entry whose body is already at buf + kEntryHead; returns its size.
size_t sealEntry(uint8_t* buf, uint8_t type, size_t bodyLen) {
  buf[0] = type;
  buf[1] = 0;
  put<uint16_t>(buf + 2, static_cast<uint16_t>(bodyLen));
  put<uint16_t>(buf + kEntryHead + bodyLen, crc16(buf, kEntryHead + bodyLen));
  return kEntryHead + bodyLen + kCrcSize;
}

size_t encodeRecord(uint8_t* buf, const RecordView& v) {
  uint8_t* b = buf + kEntryHead;
  put<uint32_t>(b, v.seq);
  b[4] = static_cast<uint8_t>(v.meta.topic);
  b[5] = static_cast<uint8_t>(v.meta.cls);
  b[6] = static_cast<uint8_t>(v.meta.timeKind);
  b[7] = 0;
  put<uint16_t>(b + 8, v.meta.dev);
  put<uint16_t>(b + 10, v.meta.bootId);
  put<uint32_t>(b + 12, v.meta.arg);
  put<uint32_t>(b + 16, v.meta.arg2);
  put<uint64_t>(b + 20, v.meta.time);
  if (v.len) memcpy(b + kRecordMeta, v.payload, v.len);
  return sealEntry(buf, kTypeRecord, kRecordMeta + v.len);
}

void decodeRecord(const uint8_t* body, size_t len, RecordView& v) {
  v.seq = get<uint32_t>(body);
  v.meta.topic = static_cast<OutTopic>(body[4]);
  v.meta.cls = static_cast<RecordClass>(body[5]);
  v.meta.timeKind = static_cast<TimeKind>(body[6]);
  v.meta.dev = get<uint16_t>(body + 8);
  v.meta.bootId = get<uint16_t>(body + 10);
  v.meta.arg = get<uint32_t>(body + 12);
  v.meta.arg2 = get<uint32_t>(body + 16);
  v.meta.time = get<uint64_t>(body + 20);
  v.payload = body + kRecordMeta;
  v.len = len - kRecordMeta;
}

}  // namespace

uint16_t crc16(const uint8_t* p, size_t n, uint16_t crc) {
  while (n--) {
    crc ^= static_cast<uint16_t>(*p++) << 8;
    for (int i = 0; i < 8; ++i) crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : crc << 1;
  }
  return crc;
}

void Journal::onRemoved(uint32_t seq, void* ctx) {
  Journal* j = static_cast<Journal*>(ctx);
  if (j->removedCount_ < VL_OUTBOX_REMOVALS) {
    j->removed_[j->removedCount_++] = seq;
  } else {
    ++j->removalsLost_;
  }
}

Result Journal::fail(Result r, const char* what) {
  failed_ = true;
  VL_LOGE("outbox store: %s failed (%s); saving stops until restart", what, toString(r));
  return Result::Rejected;
}

size_t Journal::freeCount() const {
  size_t n = 0;
  for (uint8_t i = 0; i < segCount_; ++i) n += segs_[i].used ? 0 : 1;
  return n;
}

size_t Journal::segmentsUsed() const { return segCount_ - freeCount(); }

int Journal::oldest() const {
  int best = -1;
  for (uint8_t i = 0; i < segCount_; ++i) {
    if (segs_[i].used && i != head_ && (best < 0 || segs_[i].gen < segs_[best].gen)) best = i;
  }
  return best;
}

Result Journal::readHeader(uint8_t seg, uint32_t& gen) {
  size_t got = 0;
  Result r = store_.read(seg, 0, scratch_, kHeaderEntry, got);
  if (r != Result::Ok) return r;
  if (got < kHeaderEntry || scratch_[0] != kTypeHeader || scratch_[1] != 0 ||
      get<uint16_t>(scratch_ + 2) != 8 || get<uint16_t>(scratch_ + kEntryHead + 8) != crc16(scratch_, kEntryHead + 8) ||
      get<uint32_t>(scratch_ + kEntryHead) != kMagic) {
    return Result::NotReady;  // free: erased, torn or foreign
  }
  gen = get<uint32_t>(scratch_ + kEntryHead + 4);
  return Result::Ok;
}

bool Journal::erasedFrom(uint8_t seg, size_t off) {
  while (off < segSize_) {
    size_t n = segSize_ - off < scratchCap_ ? segSize_ - off : scratchCap_;
    size_t got = 0;
    if (store_.read(seg, off, scratch_, n, got) != Result::Ok) return false;
    if (got == 0) return true;  // end of file
    for (size_t i = 0; i < got; ++i) {
      if (scratch_[i] != kErased) return false;
    }
    off += got;
  }
  return true;
}

// Walks seg's entries up to the end of its data or a torn tail. With apply,
// records and removals go into the ring. end is where the good data stops;
// clean means everything after it is unwritten, so appending there is safe.
Result Journal::scan(uint8_t seg, bool apply, size_t& end, bool& clean, uint32_t& maxSeq) {
  size_t off = 0;
  clean = false;
  for (;;) {
    if (off + kEntryHead + kCrcSize > segSize_) {
      clean = erasedFrom(seg, off);
      break;
    }
    size_t got = 0;
    Result r = store_.read(seg, off, scratch_, kEntryHead, got);
    if (r != Result::Ok) return fail(r, "read");
    if (got == 0) {
      clean = true;
      break;
    }
    if (got == kEntryHead && scratch_[0] == kErased) {
      clean = erasedFrom(seg, off);
      break;
    }
    if (got < kEntryHead || scratch_[1] != 0) break;
    size_t len = get<uint16_t>(scratch_ + 2);
    size_t total = kEntryHead + len + kCrcSize;
    if (off + total > segSize_ || total > scratchCap_) break;
    r = store_.read(seg, off + kEntryHead, scratch_ + kEntryHead, len + kCrcSize, got);
    if (r != Result::Ok) return fail(r, "read");
    if (got < len + kCrcSize || get<uint16_t>(scratch_ + kEntryHead + len) != crc16(scratch_, kEntryHead + len)) break;

    uint8_t type = scratch_[0];
    const uint8_t* body = scratch_ + kEntryHead;
    if (off == 0 && type != kTypeHeader) break;
    if (apply && type == kTypeRecord && len >= kRecordMeta) {
      RecordView v;
      decodeRecord(body, len, v);
      if (v.seq > maxSeq) maxSeq = v.seq;
      // Uptime of an earlier boot cannot be converted any more.
      if (v.meta.timeKind == TimeKind::Uptime) v.meta.timeKind = TimeKind::None;
      Result rr;
      {
        LockGuard g(*lock_);
        rr = outbox_->restore(v, seg);
      }
      if (rr != Result::Ok) {
        VL_LOGW("outbox store: saved record seq=%lu not restored (%s)", static_cast<unsigned long>(v.seq), toString(rr));
      }
    } else if (apply && type == kTypeRemovals) {
      LockGuard g(*lock_);
      for (size_t i = 0; i + 4 <= len; i += 4) {
        uint32_t seq = get<uint32_t>(body + i);
        if (seq > maxSeq) maxSeq = seq;
        outbox_->forget(seq);
      }
    }
    off += total;
  }
  end = off;
  return Result::Ok;
}

Result Journal::begin(Outbox& outbox, ILock& lock) {
  end();
  size_t count = store_.segmentCount();
  segSize_ = store_.segmentSize();
  if (count < 3 || count > VL_OUTBOX_MAX_SEGMENTS || segSize_ < kHeaderEntry + kRecordOverhead ||
      segSize_ > 0xFFFF || count * segSize_ < 2 * outbox.capacity() + 2 * segSize_ || scratchCap_ < kRemovalsEntry) {
    VL_LOGE("outbox store: needs 3-%u segments and at least twice the ring's %lu bytes", VL_OUTBOX_MAX_SEGMENTS,
            static_cast<unsigned long>(outbox.capacity()));
    return Result::Rejected;
  }
  segCount_ = static_cast<uint8_t>(count);
  outbox_ = &outbox;
  lock_ = &lock;
  failed_ = false;
  dirty_ = false;
  retryAt_ = 0;
  removedCount_ = 0;
  removalsLost_ = 0;
  maxGen_ = 0;
  headOpen_ = false;
  headDirty_ = false;
  {
    LockGuard g(*lock_);
    outbox_->setRemoveHook(onRemoved, this);
  }

  uint8_t newest = kNoSeg;
  for (uint8_t i = 0; i < segCount_; ++i) {
    uint32_t gen = 0;
    Result r = readHeader(i, gen);
    if (r != Result::Ok && r != Result::NotReady) return fail(r, "read");
    segs_[i].used = r == Result::Ok;
    segs_[i].gen = gen;
    if (segs_[i].used && (newest == kNoSeg || gen > segs_[newest].gen)) newest = i;
    if (segs_[i].used && gen > maxGen_) maxGen_ = gen;
  }

  size_t end = 0;
  bool clean = false;
  uint32_t maxSeq = 0;
  if (newest != kNoSeg && freeCount() == 0) {
    // Only a power cut while reclaiming leaves no free segment. If the newest
    // one is torn it holds nothing but copies of the oldest one's records,
    // which is still intact: drop it. Otherwise the reclaim is finished below.
    Result r = scan(newest, false, end, clean, maxSeq);
    if (r != Result::Ok) return r;
    if (!clean) {
      r = store_.erase(newest);
      if (r != Result::Ok) return fail(r, "erase");
      segs_[newest].used = false;
      newest = kNoSeg;
      for (uint8_t i = 0; i < segCount_; ++i) {
        if (segs_[i].used && (newest == kNoSeg || segs_[i].gen > segs_[newest].gen)) newest = i;
      }
    }
  }

  // Replay oldest generation first.
  uint32_t last = 0;
  bool first = true;
  for (;;) {
    uint8_t next = kNoSeg;
    for (uint8_t i = 0; i < segCount_; ++i) {
      if (segs_[i].used && (first || segs_[i].gen > last) && (next == kNoSeg || segs_[i].gen < segs_[next].gen)) {
        next = i;
      }
    }
    if (next == kNoSeg) break;
    first = false;
    last = segs_[next].gen;
    size_t segEnd = 0;
    bool segClean = false;
    Result r = scan(next, true, segEnd, segClean, maxSeq);
    if (r != Result::Ok) return r;
    if (next == newest) {
      end = segEnd;
      clean = segClean;
    }
  }
  {
    LockGuard g(*lock_);
    outbox_->finishRestore(maxSeq);
  }

  if (newest != kNoSeg) {
    head_ = newest;
    headOff_ = end;
    headOpen_ = clean;
  }
  if (freeCount() == 0) {
    int victim = oldest();
    if (victim >= 0) return reclaim(static_cast<uint8_t>(victim));
  }
  return Result::Ok;
}

void Journal::end() {
  if (!outbox_) return;
  {
    LockGuard g(*lock_);
    outbox_->setRemoveHook(nullptr, nullptr);
  }
  outbox_ = nullptr;
}

Result Journal::writeHead(size_t len) {
  Result r = store_.write(head_, headOff_, scratch_, len);
  if (r != Result::Ok) return fail(r, "write");
  headOff_ += len;
  headDirty_ = true;
  return Result::Ok;
}

Result Journal::syncHead() {
  if (!headDirty_) return Result::Ok;
  Result r = store_.sync(head_);
  if (r != Result::Ok) return fail(r, "sync");
  headDirty_ = false;
  return Result::Ok;
}

// Starts a new head segment. If that used the last free one, the oldest
// segment's live records are copied into it and the oldest is erased, so a
// free segment always remains for the next reclaim.
Result Journal::openNext() {
  Result r = syncHead();
  if (r != Result::Ok) return r;
  uint8_t seg = kNoSeg;
  for (uint8_t i = 0; i < segCount_ && seg == kNoSeg; ++i) {
    if (!segs_[i].used) seg = i;
  }
  if (seg == kNoSeg) return Result::QueueFull;
  r = store_.erase(seg);
  if (r != Result::Ok) return fail(r, "erase");
  put<uint32_t>(scratch_ + kEntryHead, kMagic);
  put<uint32_t>(scratch_ + kEntryHead + 4, maxGen_ + 1);
  size_t len = sealEntry(scratch_, kTypeHeader, 8);
  r = store_.write(seg, 0, scratch_, len);
  if (r != Result::Ok) return fail(r, "write");
  segs_[seg].used = true;
  segs_[seg].gen = ++maxGen_;
  head_ = seg;
  headOff_ = len;
  headOpen_ = true;
  headDirty_ = true;
  if (freeCount() == 0) {
    int victim = oldest();
    if (victim >= 0) return reclaim(static_cast<uint8_t>(victim));
  }
  return Result::Ok;
}

Result Journal::reclaim(uint8_t victim) {
  for (;;) {
    size_t len = 0;
    {
      LockGuard g(*lock_);
      RecordView v;
      if (!outbox_->nextInSegment(victim, v)) break;
      if (headOff_ + kRecordOverhead + v.len > segSize_) return Result::QueueFull;  // cannot happen: see openNext()
      len = encodeRecord(scratch_, v);
      outbox_->setSaved(v.seq, head_);
    }
    Result r = writeHead(len);
    if (r != Result::Ok) return r;
  }
  // The copies must be durable before the originals go.
  Result r = syncHead();
  if (r != Result::Ok) return r;
  r = store_.erase(victim);
  if (r != Result::Ok) return fail(r, "erase");
  segs_[victim].used = false;
  return Result::Ok;
}

Result Journal::flush() {
  if (!outbox_ || failed_) return Result::Rejected;
  Result r;

  // Removals first: then the store never holds more live records than the
  // ring did at some moment, so a restore fits.
  bool removals;
  {
    LockGuard g(*lock_);
    removals = removedCount_ > 0;
  }
  if (removals && (!headOpen_ || headOff_ + kRemovalsEntry > segSize_)) {
    r = openNext();
    if (r != Result::Ok) return r;
  }
  size_t n = 0;
  uint32_t lost = 0;
  {
    LockGuard g(*lock_);
    n = removedCount_;
    for (size_t i = 0; i < n; ++i) put<uint32_t>(scratch_ + kEntryHead + 4 * i, removed_[i]);
    removedCount_ = 0;
    lost = removalsLost_;
    removalsLost_ = 0;
  }
  if (lost) {
    VL_LOGW("outbox store: %lu removals not saved; those records may be sent again after a restart",
            static_cast<unsigned long>(lost));
  }
  if (n) {
    r = writeHead(sealEntry(scratch_, kTypeRemovals, 4 * n));
    if (r != Result::Ok) return r;
  }

  size_t opens = 0;
  for (;;) {
    size_t len = 0;
    bool ready = false;
    bool skipped = false;
    {
      LockGuard g(*lock_);
      RecordView v;
      if (!outbox_->nextUnsaved(v)) break;
      size_t need = kRecordOverhead + v.len;
      if (need > scratchCap_ || need > segSize_ - kHeaderEntry) {
        outbox_->setSaved(v.seq, Outbox::kNotSaved);
        skipped = true;
      } else if (headOpen_ && headOff_ + need <= segSize_) {
        len = encodeRecord(scratch_, v);
        outbox_->setSaved(v.seq, head_);
        ready = true;
      }
    }
    if (skipped) {
      VL_LOGW("outbox store: record too large to save; it is kept in RAM only");
      continue;
    }
    if (!ready) {
      if (++opens > segCount_) {
        VL_LOGW("outbox store: full of unacknowledged records");
        syncHead();
        return Result::QueueFull;
      }
      r = openNext();
      if (r != Result::Ok) return r;
      continue;
    }
    r = writeHead(len);
    if (r != Result::Ok) return r;
  }
  return syncHead();
}

void Journal::tick(uint64_t nowMs) {
  if (!outbox_ || failed_) return;
  bool due = false;
  {
    LockGuard g(*lock_);
    size_t pending = outbox_->unsaved();
    if (pending == 0 && removedCount_ == 0) {
      dirty_ = false;
      return;
    }
    if (!dirty_) {
      dirty_ = true;
      dirtySince_ = nowMs;
    }
    due = outbox_->unsavedControl() > 0 || pending >= VL_OUTBOX_FLUSH_RECORDS ||
          removedCount_ >= VL_OUTBOX_REMOVALS / 2 || nowMs - dirtySince_ >= VL_OUTBOX_FLUSH_MS;
  }
  if (!due || nowMs < retryAt_) return;
  Result r = flush();
  if (r == Result::QueueFull) {
    retryAt_ = nowMs + VL_OUTBOX_FLUSH_MS;
  } else if (r == Result::Ok) {
    dirty_ = false;
  }
}

}  // namespace vl

#endif  // VL_ENABLE_PERSISTENCE
