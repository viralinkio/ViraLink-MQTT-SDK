#include "vl/platform/esp8266/FsStore.h"

#if VL_PLATFORM_ESP8266

#include <stdio.h>
#include <string.h>

#include "vl/Log.h"

namespace vl {

FsStore::FsStore(fs::FS& fs, const char* dir) : fs_(fs) {
  snprintf(dir_, sizeof(dir_), "%s", dir ? dir : "/vl");
}

bool FsStore::path(const char* key, const char* suffix, char* out, size_t cap) const {
  size_t len = key ? strlen(key) : 0;
  if (len == 0 || len > kMaxKey || strchr(key, '/')) return false;
  int n = snprintf(out, cap, "%s/%s%s", dir_, key, suffix);
  return n > 0 && static_cast<size_t>(n) < cap;
}

Result FsStore::get(const char* key, void* buf, size_t cap, size_t& len) {
  char p[kPathSize];
  if (!path(key, "", p, sizeof(p)) || !fs_.exists(p)) return Result::Rejected;
  fs::File f = fs_.open(p, "r");
  if (!f) return Result::Rejected;
  size_t size = f.size();
  if (size > cap) {
    f.close();
    return Result::TooLarge;
  }
  int got = f.read(static_cast<uint8_t*>(buf), size);
  f.close();
  if (got < 0 || static_cast<size_t>(got) != size) return Result::Rejected;
  len = size;
  return Result::Ok;
}

Result FsStore::set(const char* key, const void* value, size_t len) {
  char p[kPathSize], tmp[kPathSize];
  if (!path(key, "", p, sizeof(p)) || !path(key, ".tmp", tmp, sizeof(tmp))) return Result::Rejected;
  if (!fs_.exists(dir_)) fs_.mkdir(dir_);
  fs::File f = fs_.open(tmp, "w");
  if (!f) {
    VL_LOGW("store: cannot write %s", tmp);
    return Result::Rejected;
  }
  size_t put = len ? f.write(static_cast<const uint8_t*>(value), len) : 0;
  f.close();
  if (put != len || !fs_.rename(tmp, p)) {
    fs_.remove(tmp);
    VL_LOGW("store: cannot save %s", p);
    return Result::Rejected;
  }
  return Result::Ok;
}

Result FsStore::erase(const char* key) {
  char p[kPathSize];
  if (!path(key, "", p, sizeof(p))) return Result::Rejected;
  if (!fs_.exists(p)) return Result::Ok;
  return fs_.remove(p) ? Result::Ok : Result::Rejected;
}

}  // namespace vl

#endif  // VL_PLATFORM_ESP8266
