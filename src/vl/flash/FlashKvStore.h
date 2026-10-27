#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vl/flash/Flash.h"
#include "vl/pal/KeyValueStore.h"

namespace vl {

// IKeyValueStore on NOR flash that survives power cuts: a cut during set() or
// erase() leaves the old value or the new one, never a torn one. The flash
// (give it a FlashRegion of two or more erase blocks) is split into two banks.
// The active bank is an append-only log in which the last record for a key
// wins. When it fills up, the live records are copied into the other bank,
// whose header, written last, makes it the active one. Each set() of a new
// value costs one record (key + value + 5 bytes); setting the stored value
// again writes nothing. Values are stored in the clear. The flash is first
// read on the first call, so it may be started after construction.
class FlashKvStore : public IKeyValueStore {
 public:
  explicit FlashKvStore(IFlash& flash) : flash_(flash) {}

  // NotReady while the flash has no usable size; NoMemory when the live
  // values and the new one do not fit in a bank.
  Result get(const char* key, void* buf, size_t cap, size_t& len) override;
  Result set(const char* key, const void* value, size_t len) override;
  Result erase(const char* key) override;

 private:
  static const size_t kHeader = 10;  // magic u32, generation u32, crc16
  static const size_t kRecordHead = 3;  // keyLen (bit 7: deleted) u8, valLen u16
  static const size_t kMaxKey = 15;
  static const uint8_t kDeleted = 0x80;

  struct Rec {
    uint32_t off;     // in the bank
    uint8_t keyLen;   // without the flag
    bool deleted;
    uint16_t valLen;
    uint32_t size() const { return static_cast<uint32_t>(kRecordHead) + keyLen + valLen + 2; }
  };

  Result mount();
  Result format();
  // The record at off in bank: 1 = valid, 0 = the log ends (erased or torn), -1 = read error.
  int recordAt(uint32_t bank, uint32_t off, Rec& r);
  bool keyIs(uint32_t bank, const Rec& r, const char* key, size_t keyLen, Result& err);
  // The last record for key in the active log; false when there is none.
  bool find(const char* key, size_t keyLen, Rec& r, Result& err);
  bool sameValue(const Rec& r, const uint8_t* value, size_t len, Result& err);
  Result write(const char* key, size_t keyLen, const uint8_t* value, size_t len, bool deleted);
  Result append(uint32_t bank, uint32_t off, const char* key, size_t keyLen, const uint8_t* value, size_t len,
                bool deleted);
  // Rewrites the live records, minus key, into the other bank, then the new
  // record unless it deletes.
  Result compact(const char* key, size_t keyLen, const uint8_t* value, size_t len, bool deleted);
  Result copy(uint32_t from, uint32_t to, uint32_t n);
  uint32_t bankBase(uint32_t bank) const { return bank * bank_; }

  IFlash& flash_;
  bool mounted_ = false;
  bool dirty_ = false;  // the log ends in a torn record: compact before appending
  uint32_t bank_ = 0;   // bank size
  uint32_t active_ = 0;
  uint32_t gen_ = 0;
  uint32_t end_ = 0;  // where the next record goes in the active bank
};

}  // namespace vl
