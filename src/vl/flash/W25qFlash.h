#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vl/flash/Flash.h"
#include "vl/pal/Clock.h"
#include "vl/pal/Spi.h"

namespace vl {

// IFlash driver for SPI NOR chips with the common JEDEC command set: Winbond
// W25Q, GigaDevice GD25Q, Macronix MX25L and others. 4 KB sector erase,
// 256-byte pages, 3-byte addresses: chips over 16 MB are used for their first
// 16 MB. Every program and erase waits for the chip to finish, polling and
// yielding: up to 10 ms per page, 1 s per sector.
class W25qFlash : public IFlash {
 public:
  // clock null: the platform clock.
  explicit W25qFlash(ISpiDevice& spi, IClock* clock = nullptr);

  // Wakes the chip from deep power-down and identifies it. Rejected when
  // nothing answers. Call after the SPI bus is started; until then, size()
  // is 0.
  Result begin();
  // Manufacturer, memory type and capacity bytes from begin(), or 0.
  uint32_t jedecId() const { return id_; }

  uint32_t size() const override { return size_; }
  uint32_t eraseSize() const override { return kSector; }
  Result read(uint32_t addr, uint8_t* buf, size_t n) override;
  // Timeout if the chip stays busy; Rejected if it ignores write enable
  // (write-protected).
  Result program(uint32_t addr, const uint8_t* p, size_t n) override;
  Result erase(uint32_t addr) override;

 private:
  static const uint32_t kSector = 4096;
  static const uint32_t kPage = 256;

  uint32_t readId();
  uint8_t status();
  Result writeEnable();
  bool waitReady(uint32_t timeoutMs);
  void command(uint8_t op, uint32_t addr);
  bool inside(uint32_t addr, size_t n) const { return addr <= size_ && n <= size_ - addr; }

  ISpiDevice& spi_;
  IClock* clock_;
  uint32_t size_ = 0;
  uint32_t id_ = 0;
};

}  // namespace vl
