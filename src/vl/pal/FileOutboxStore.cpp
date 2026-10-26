#include "vl/pal/FileOutboxStore.h"

#if VL_HAS_FILE_STORE

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "vl/Log.h"

namespace vl {

FileOutboxStore::FileOutboxStore(const char* dir, size_t segments, size_t segmentSize)
    : count_(segments), size_(segmentSize) {
  if (dir && strlen(dir) < sizeof(dir_)) {
    memcpy(dir_, dir, strlen(dir) + 1);
  } else {
    count_ = 0;  // unusable: Persistence rejects it
    VL_LOGE("outbox store: directory name too long");
  }
}

FileOutboxStore::~FileOutboxStore() { closeWriter(); }

bool FileOutboxStore::path(size_t seg, char* out, size_t cap) const {
  int n = snprintf(out, cap, "%s/ob%u", dir_, static_cast<unsigned>(seg));
  return n > 0 && static_cast<size_t>(n) < cap && seg < count_;
}

void FileOutboxStore::closeWriter() {
  if (writer_) fclose(writer_);
  writer_ = nullptr;
}

Result FileOutboxStore::read(size_t seg, size_t off, uint8_t* buf, size_t n, size_t& got) {
  got = 0;
  char p[64];
  if (!path(seg, p, sizeof(p))) return Result::Rejected;
  if (writer_ && writerSeg_ == seg) fflush(writer_);
  FILE* f = fopen(p, "rb");
  if (!f) return errno == ENOENT ? Result::Ok : Result::Rejected;  // never written: empty
  Result r = Result::Ok;
  if (fseek(f, static_cast<long>(off), SEEK_SET) != 0) {
    r = Result::Rejected;
  } else {
    got = fread(buf, 1, n, f);
    if (got < n && ferror(f)) r = Result::Rejected;
  }
  fclose(f);
  return r;
}

Result FileOutboxStore::write(size_t seg, size_t off, const uint8_t* p, size_t n) {
  if (off + n > size_) return Result::Rejected;
  if (writer_ && writerSeg_ != seg) closeWriter();
  if (!writer_) {
    char name[64];
    if (!path(seg, name, sizeof(name))) return Result::Rejected;
    writer_ = fopen(name, "r+b");
    if (!writer_) writer_ = fopen(name, "w+b");
    if (!writer_) return Result::Rejected;
    writerSeg_ = seg;
  }
  if (fseek(writer_, static_cast<long>(off), SEEK_SET) != 0 || fwrite(p, 1, n, writer_) != n) {
    closeWriter();
    return Result::Rejected;
  }
  return Result::Ok;
}

Result FileOutboxStore::erase(size_t seg) {
  char p[64];
  if (!path(seg, p, sizeof(p))) return Result::Rejected;
  if (writer_ && writerSeg_ == seg) closeWriter();
  if (remove(p) != 0 && errno != ENOENT) return Result::Rejected;
  return Result::Ok;
}

Result FileOutboxStore::sync(size_t seg) {
  if (!writer_ || writerSeg_ != seg) return Result::Ok;
  if (fflush(writer_) != 0 || fsync(fileno(writer_)) != 0) return Result::Rejected;
  return Result::Ok;
}

}  // namespace vl

#endif  // VL_HAS_FILE_STORE
