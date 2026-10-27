#include "vl/flash/FlashKvStore.h"

#include <string.h>

#include "vl/Log.h"
#include "vl/core/Crc16.h"

namespace vl {

namespace {

const uint32_t kMagic = 0x314B4C56;  // "VLK1"
const size_t kChunk = 32;            // stack buffer for copies and compares

void putLe32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

Result FlashKvStore::mount() {
  if (mounted_) return Result::Ok;
  uint32_t block = flash_.eraseSize();
  uint32_t size = flash_.size();
  if (block == 0 || size / 2 < block) return Result::NotReady;
  bank_ = size / 2 / block * block;

  bool valid[2];
  uint32_t gen[2];
  for (uint32_t b = 0; b < 2; ++b) {
    uint8_t h[kHeader];
    // A read error must not look like blank flash, or it would be formatted.
    if (flash_.read(bankBase(b), h, sizeof(h)) != Result::Ok) return Result::Rejected;
    uint16_t crc = static_cast<uint16_t>(h[8] | (h[9] << 8));
    valid[b] = le32(h) == kMagic && crc16(h, 8) == crc;
    gen[b] = le32(h + 4);
  }
  if (!valid[0] && !valid[1]) return format();
  if (valid[0] && valid[1]) {
    active_ = static_cast<int32_t>(gen[1] - gen[0]) > 0 ? 1 : 0;
  } else {
    active_ = valid[0] ? 0 : 1;
  }
  gen_ = gen[active_];

  uint32_t off = kHeader;
  for (;;) {
    Rec r;
    int rc = recordAt(active_, off, r);
    if (rc < 0) return Result::Rejected;
    if (rc == 0) break;
    off += r.size();
  }
  end_ = off;
  // A cut page program can leave bits set past what reads as the end.
  dirty_ = false;
  uint8_t buf[kChunk];
  for (uint32_t a = end_; a < bank_ && !dirty_; a += kChunk) {
    size_t n = bank_ - a < kChunk ? bank_ - a : kChunk;
    if (flash_.read(bankBase(active_) + a, buf, n) != Result::Ok) return Result::Rejected;
    for (size_t i = 0; i < n; ++i) dirty_ = dirty_ || buf[i] != 0xFF;
  }
  mounted_ = true;
  return Result::Ok;
}

Result FlashKvStore::format() {
  for (uint32_t a = 0; a < bank_; a += flash_.eraseSize()) {
    Result r = flash_.erase(a);
    if (r != Result::Ok) return r;
  }
  uint8_t h[kHeader];
  putLe32(h, kMagic);
  putLe32(h + 4, 1);
  uint16_t crc = crc16(h, 8);
  h[8] = static_cast<uint8_t>(crc);
  h[9] = static_cast<uint8_t>(crc >> 8);
  Result r = flash_.program(0, h, sizeof(h));
  if (r != Result::Ok) return r;
  active_ = 0;
  gen_ = 1;
  end_ = kHeader;
  dirty_ = false;
  mounted_ = true;
  return Result::Ok;
}

int FlashKvStore::recordAt(uint32_t bank, uint32_t off, Rec& r) {
  if (off + kRecordHead + 2 > bank_) return 0;
  uint32_t base = bankBase(bank) + off;
  uint8_t h[kRecordHead];
  if (flash_.read(base, h, sizeof(h)) != Result::Ok) return -1;
  r.off = off;
  r.keyLen = h[0] & 0x7F;
  r.deleted = (h[0] & kDeleted) != 0;
  r.valLen = static_cast<uint16_t>(h[1] | (h[2] << 8));
  if (r.keyLen == 0 || r.keyLen > kMaxKey || off + r.size() > bank_) return 0;
  uint16_t crc = crc16(h, sizeof(h));
  uint8_t buf[kChunk];
  uint32_t body = r.keyLen + r.valLen;
  for (uint32_t done = 0; done < body;) {
    size_t n = body - done < kChunk ? body - done : kChunk;
    if (flash_.read(base + kRecordHead + done, buf, n) != Result::Ok) return -1;
    crc = crc16(buf, n, crc);
    done += static_cast<uint32_t>(n);
  }
  uint8_t c[2];
  if (flash_.read(base + kRecordHead + body, c, 2) != Result::Ok) return -1;
  return crc == static_cast<uint16_t>(c[0] | (c[1] << 8)) ? 1 : 0;
}

bool FlashKvStore::keyIs(uint32_t bank, const Rec& r, const char* key, size_t keyLen, Result& err) {
  if (r.keyLen != keyLen) return false;
  char k[kMaxKey];
  if (flash_.read(bankBase(bank) + r.off + kRecordHead, reinterpret_cast<uint8_t*>(k), keyLen) != Result::Ok) {
    err = Result::Rejected;
    return false;
  }
  return memcmp(k, key, keyLen) == 0;
}

// Records up to end_ were checked at mount or written here, so only their
// heads are read.
bool FlashKvStore::find(const char* key, size_t keyLen, Rec& found, Result& err) {
  bool any = false;
  uint8_t h[kRecordHead];
  for (uint32_t off = kHeader; off < end_;) {
    if (flash_.read(bankBase(active_) + off, h, sizeof(h)) != Result::Ok) {
      err = Result::Rejected;
      return false;
    }
    Rec r;
    r.off = off;
    r.keyLen = h[0] & 0x7F;
    r.deleted = (h[0] & kDeleted) != 0;
    r.valLen = static_cast<uint16_t>(h[1] | (h[2] << 8));
    if (keyIs(active_, r, key, keyLen, err)) {
      found = r;
      any = true;
    }
    if (err != Result::Ok) return false;
    off += r.size();
  }
  return any && !found.deleted;
}

bool FlashKvStore::sameValue(const Rec& r, const uint8_t* value, size_t len, Result& err) {
  if (r.valLen != len) return false;
  uint32_t base = bankBase(active_) + r.off + kRecordHead + r.keyLen;
  uint8_t buf[kChunk];
  for (size_t done = 0; done < len;) {
    size_t n = len - done < kChunk ? len - done : kChunk;
    if (flash_.read(static_cast<uint32_t>(base + done), buf, n) != Result::Ok) {
      err = Result::Rejected;
      return false;
    }
    if (memcmp(buf, value + done, n) != 0) return false;
    done += n;
  }
  return true;
}

Result FlashKvStore::get(const char* key, void* buf, size_t cap, size_t& len) {
  size_t keyLen = key ? strlen(key) : 0;
  if (keyLen == 0 || keyLen > kMaxKey) return Result::Rejected;
  Result r = mount();
  if (r != Result::Ok) return r;
  Rec rec;
  Result err = Result::Ok;
  if (!find(key, keyLen, rec, err)) return err == Result::Ok ? Result::Rejected : err;
  if (rec.valLen > cap) return Result::TooLarge;
  if (rec.valLen &&
      flash_.read(bankBase(active_) + rec.off + kRecordHead + rec.keyLen, static_cast<uint8_t*>(buf), rec.valLen) !=
          Result::Ok) {
    return Result::Rejected;
  }
  len = rec.valLen;
  return Result::Ok;
}

Result FlashKvStore::set(const char* key, const void* value, size_t len) {
  if (len && !value) return Result::Rejected;
  return write(key, key ? strlen(key) : 0, static_cast<const uint8_t*>(value), len, false);
}

Result FlashKvStore::erase(const char* key) { return write(key, key ? strlen(key) : 0, nullptr, 0, true); }

Result FlashKvStore::write(const char* key, size_t keyLen, const uint8_t* value, size_t len, bool deleted) {
  if (keyLen == 0 || keyLen > kMaxKey) return Result::Rejected;
  if (len > 0xFFFF) return Result::NoMemory;
  Result r = mount();
  if (r != Result::Ok) return r;
  Rec cur;
  Result err = Result::Ok;
  bool exists = find(key, keyLen, cur, err);
  if (err != Result::Ok) return err;
  if (deleted ? !exists : exists && sameValue(cur, value, len, err)) return Result::Ok;
  if (err != Result::Ok) return err;
  uint32_t size = static_cast<uint32_t>(kRecordHead + keyLen + len + 2);
  if (kHeader + size > bank_) return Result::NoMemory;
  if (dirty_ || end_ + size > bank_) return compact(key, keyLen, value, len, deleted);
  r = append(active_, end_, key, keyLen, value, len, deleted);
  if (r == Result::Ok) {
    end_ += size;
  } else {
    dirty_ = true;  // part of the record may be programmed
  }
  return r;
}

Result FlashKvStore::append(uint32_t bank, uint32_t off, const char* key, size_t keyLen, const uint8_t* value,
                            size_t len, bool deleted) {
  uint8_t head[kRecordHead + kMaxKey];
  head[0] = static_cast<uint8_t>(keyLen | (deleted ? kDeleted : 0));
  head[1] = static_cast<uint8_t>(len);
  head[2] = static_cast<uint8_t>(len >> 8);
  memcpy(head + kRecordHead, key, keyLen);
  size_t headLen = kRecordHead + keyLen;
  uint16_t crc = crc16(head, headLen);
  if (len) crc = crc16(value, len, crc);
  uint8_t c[2] = {static_cast<uint8_t>(crc), static_cast<uint8_t>(crc >> 8)};
  uint32_t base = bankBase(bank) + off;
  Result r = flash_.program(base, head, headLen);
  if (r == Result::Ok && len) r = flash_.program(static_cast<uint32_t>(base + headLen), value, len);
  if (r == Result::Ok) r = flash_.program(static_cast<uint32_t>(base + headLen + len), c, 2);
  return r;
}

Result FlashKvStore::compact(const char* key, size_t keyLen, const uint8_t* value, size_t len, bool deleted) {
  // Two passes over the live records: size them, then copy them. A record is
  // live when it is not a deletion, not for key, and not superseded later.
  uint32_t other = 1 - active_;
  uint32_t pos = kHeader;
  for (int pass = 0; pass < 2; ++pass) {
    if (pass == 1) {
      if (pos + (deleted ? 0 : kRecordHead + keyLen + len + 2) > bank_) {
        VL_LOGW("kv store: full");
        return Result::NoMemory;
      }
      for (uint32_t a = 0; a < bank_; a += flash_.eraseSize()) {
        Result r = flash_.erase(bankBase(other) + a);
        if (r != Result::Ok) return r;
      }
      pos = kHeader;
    }
    for (uint32_t off = kHeader; off < end_;) {
      Rec r;
      int rc = recordAt(active_, off, r);
      if (rc <= 0) return Result::Rejected;  // checked at mount: the flash changed under us
      off += r.size();
      Result err = Result::Ok;
      if (r.deleted || keyIs(active_, r, key, keyLen, err)) continue;
      char k[kMaxKey];
      if (err != Result::Ok ||
          flash_.read(bankBase(active_) + r.off + kRecordHead, reinterpret_cast<uint8_t*>(k), r.keyLen) != Result::Ok) {
        return Result::Rejected;
      }
      bool superseded = false;
      for (uint32_t later = off; later < end_ && !superseded;) {
        Rec l;
        if (recordAt(active_, later, l) <= 0) return Result::Rejected;
        superseded = keyIs(active_, l, k, r.keyLen, err);
        if (err != Result::Ok) return err;
        later += l.size();
      }
      if (superseded) continue;
      if (pass == 1) {
        Result c = copy(bankBase(active_) + r.off, bankBase(other) + pos, r.size());
        if (c != Result::Ok) return c;
      }
      pos += r.size();
    }
  }
  if (!deleted) {
    Result r = append(other, pos, key, keyLen, value, len, false);
    if (r != Result::Ok) return r;
    pos += static_cast<uint32_t>(kRecordHead + keyLen + len + 2);
  }
  // The header is the commit point: until it is whole, the old bank wins.
  uint8_t h[kHeader];
  putLe32(h, kMagic);
  putLe32(h + 4, gen_ + 1);
  uint16_t crc = crc16(h, 8);
  h[8] = static_cast<uint8_t>(crc);
  h[9] = static_cast<uint8_t>(crc >> 8);
  Result r = flash_.program(bankBase(other), h, sizeof(h));
  if (r != Result::Ok) return r;
  active_ = other;
  gen_ += 1;
  end_ = pos;
  dirty_ = false;
  return Result::Ok;
}

Result FlashKvStore::copy(uint32_t from, uint32_t to, uint32_t n) {
  uint8_t buf[kChunk];
  for (uint32_t done = 0; done < n;) {
    size_t k = n - done < kChunk ? n - done : kChunk;
    Result r = flash_.read(from + done, buf, k);
    if (r == Result::Ok) r = flash_.program(to + done, buf, k);
    if (r != Result::Ok) return r;
    done += static_cast<uint32_t>(k);
  }
  return Result::Ok;
}

}  // namespace vl
