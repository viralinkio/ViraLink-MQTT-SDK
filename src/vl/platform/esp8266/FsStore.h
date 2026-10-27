#pragma once

#include "vl/Config.h"

#if VL_PLATFORM_ESP8266

#include <FS.h>
#include <stddef.h>

#include "vl/pal/KeyValueStore.h"

namespace vl {

// IKeyValueStore over an Arduino file system the application mounts, such as
// LittleFS after LittleFS.begin(): one file per key in dir. A value is written
// to a temporary file and renamed over the old one, which LittleFS does
// atomically, so a power cut leaves the old value or the new one. Values are
// stored in the clear.
class FsStore : public IKeyValueStore {
 public:
  // dir is copied (at most 23 characters) and created on the first set().
  explicit FsStore(fs::FS& fs, const char* dir = "/vl");

  Result get(const char* key, void* buf, size_t cap, size_t& len) override;
  Result set(const char* key, const void* value, size_t len) override;
  Result erase(const char* key) override;

 private:
  static const size_t kMaxKey = 15;  // the IKeyValueStore limit
  static const size_t kDirSize = 24;
  static const size_t kPathSize = kDirSize + kMaxKey + 8;  // dir/key.tmp

  // dir/key, plus suffix; false if the key is empty or too long.
  bool path(const char* key, const char* suffix, char* out, size_t cap) const;

  fs::FS& fs_;
  char dir_[kDirSize] = {};
};

}  // namespace vl

#endif  // VL_PLATFORM_ESP8266
