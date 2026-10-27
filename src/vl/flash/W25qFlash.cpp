#include "vl/flash/W25qFlash.h"

#include "vl/Log.h"
#include "vl/pal/Platform.h"

namespace vl {

namespace {

const uint8_t kRead = 0x03;
const uint8_t kPageProgram = 0x02;
const uint8_t kSectorErase = 0x20;
const uint8_t kWriteEnable = 0x06;
const uint8_t kReadStatus = 0x05;
const uint8_t kJedecId = 0x9F;
const uint8_t kRelease = 0xAB;  // release from deep power-down

const uint8_t kBusy = 0x01;
const uint8_t kWel = 0x02;

const uint32_t kPageMs = 10;     // tPP is 3 ms or less on common parts
const uint32_t kSectorMs = 1000;  // tSE is 400 ms or less
const uint32_t kWakeMs = 10;      // tRES1 is microseconds; poll the ID instead of waiting blind

const uint32_t kMax3Byte = static_cast<uint32_t>(16) << 20;

}  // namespace

W25qFlash::W25qFlash(ISpiDevice& spi, IClock* clock) : spi_(spi), clock_(clock ? clock : &platformClock()) {}

Result W25qFlash::begin() {
  size_ = 0;
  id_ = 0;
  spi_.select();
  spi_.write(&kRelease, 1);
  spi_.deselect();
  uint64_t deadline = clock_->uptimeMs() + kWakeMs;
  uint32_t id;
  for (;;) {
    id = readId();
    uint8_t maker = static_cast<uint8_t>(id >> 16);
    if (maker != 0x00 && maker != 0xFF) break;  // MISO held low or floating: no chip yet
    if (clock_->uptimeMs() >= deadline) {
      VL_LOGW("flash: no SPI NOR chip answers");
      return Result::Rejected;
    }
    platformYield();
  }
  uint8_t capacity = static_cast<uint8_t>(id);
  if (capacity < 0x10 || capacity > 0x20) {
    VL_LOGW("flash: unknown chip %06lx", static_cast<unsigned long>(id));
    return Result::Rejected;
  }
  size_ = capacity >= 0x18 ? kMax3Byte : static_cast<uint32_t>(1) << capacity;
  id_ = id;
  return Result::Ok;
}

uint32_t W25qFlash::readId() {
  uint8_t b[3];
  spi_.select();
  spi_.write(&kJedecId, 1);
  spi_.read(b, sizeof(b));
  spi_.deselect();
  return (static_cast<uint32_t>(b[0]) << 16) | (static_cast<uint32_t>(b[1]) << 8) | b[2];
}

uint8_t W25qFlash::status() {
  uint8_t s;
  spi_.select();
  spi_.write(&kReadStatus, 1);
  spi_.read(&s, 1);
  spi_.deselect();
  return s;
}

Result W25qFlash::writeEnable() {
  spi_.select();
  spi_.write(&kWriteEnable, 1);
  spi_.deselect();
  if (status() & kWel) return Result::Ok;
  VL_LOGW("flash: write enable ignored (write-protected?)");
  return Result::Rejected;
}

bool W25qFlash::waitReady(uint32_t timeoutMs) {
  uint64_t deadline = clock_->uptimeMs() + timeoutMs;
  for (;;) {
    if (!(status() & kBusy)) return true;
    if (clock_->uptimeMs() >= deadline) {
      VL_LOGW("flash: chip stays busy");
      return false;
    }
    platformYield();
  }
}

// Leaves the chip selected for the data that follows.
void W25qFlash::command(uint8_t op, uint32_t addr) {
  uint8_t c[4] = {op, static_cast<uint8_t>(addr >> 16), static_cast<uint8_t>(addr >> 8), static_cast<uint8_t>(addr)};
  spi_.select();
  spi_.write(c, sizeof(c));
}

Result W25qFlash::read(uint32_t addr, uint8_t* buf, size_t n) {
  if (!size_ || !inside(addr, n)) return Result::Rejected;
  if (!n) return Result::Ok;
  command(kRead, addr);
  spi_.read(buf, n);
  spi_.deselect();
  return Result::Ok;
}

Result W25qFlash::program(uint32_t addr, const uint8_t* p, size_t n) {
  if (!size_ || !inside(addr, n)) return Result::Rejected;
  while (n) {
    // A page program wraps inside its page, so each call stays in one.
    size_t chunk = kPage - addr % kPage;
    if (chunk > n) chunk = n;
    Result r = writeEnable();
    if (r != Result::Ok) return r;
    command(kPageProgram, addr);
    spi_.write(p, chunk);
    spi_.deselect();
    if (!waitReady(kPageMs)) return Result::Timeout;
    addr += static_cast<uint32_t>(chunk);
    p += chunk;
    n -= chunk;
  }
  return Result::Ok;
}

Result W25qFlash::erase(uint32_t addr) {
  if (!size_ || addr >= size_ || addr % kSector) return Result::Rejected;
  Result r = writeEnable();
  if (r != Result::Ok) return r;
  command(kSectorErase, addr);
  spi_.deselect();
  return waitReady(kSectorMs) ? Result::Ok : Result::Timeout;
}

}  // namespace vl
