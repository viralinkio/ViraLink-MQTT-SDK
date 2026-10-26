#pragma once

#include "vl/Config.h"

// Needs a POSIX file API: ESP32 (VFS) and host builds.
#if VL_ENABLE_PERSISTENCE && (VL_PLATFORM_ESP32 || !defined(ARDUINO))
#define VL_HAS_FILE_STORE 1

#include <stddef.h>
#include <stdio.h>

#include "vl/pal/OutboxStore.h"

namespace vl {

// One file per segment ("ob0", "ob1", ...) in a directory of a file system
// the application mounts: LittleFS (recommended: writes are atomic at each
// sync), FAT or SPIFFS. For example, on Arduino-ESP32 after
// LittleFS.begin(true): FileOutboxStore store("/littlefs").
class FileOutboxStore : public IOutboxStore {
 public:
  // dir is copied (at most 47 characters) and must exist.
  explicit FileOutboxStore(const char* dir, size_t segments = 8, size_t segmentSize = 4096);
  ~FileOutboxStore() override;
  FileOutboxStore(const FileOutboxStore&) = delete;
  FileOutboxStore& operator=(const FileOutboxStore&) = delete;

  size_t segmentCount() const override { return count_; }
  size_t segmentSize() const override { return size_; }
  Result read(size_t seg, size_t off, uint8_t* buf, size_t n, size_t& got) override;
  Result write(size_t seg, size_t off, const uint8_t* p, size_t n) override;
  Result erase(size_t seg) override;
  Result sync(size_t seg) override;

 private:
  bool path(size_t seg, char* out, size_t cap) const;
  void closeWriter();

  char dir_[48] = {};
  size_t count_;
  size_t size_;
  FILE* writer_ = nullptr;  // the segment being appended to stays open
  size_t writerSeg_ = 0;
};

}  // namespace vl

#else
#define VL_HAS_FILE_STORE 0
#endif
