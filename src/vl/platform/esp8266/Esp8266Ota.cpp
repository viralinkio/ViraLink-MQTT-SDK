#include "vl/platform/esp8266/Esp8266Ota.h"

#if VL_PLATFORM_ESP8266

#include <Arduino.h>
#include <Updater.h>

#include "vl/Log.h"

namespace vl {

// Update is always begun with all the free space, so it never counts as
// complete before finish(): end(false) then discards the image, whereas once
// every declared byte is in, it would install it.
Result Esp8266FirmwareWriter::begin(size_t size) {
  abort();
  uint32_t space = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
  if (size > space) {
    VL_LOGW("ota: image of %lu bytes, %lu free", static_cast<unsigned long>(size), static_cast<unsigned long>(space));
    return Result::TooLarge;
  }
  if (!Update.begin(space)) {
    VL_LOGW("ota: Update.begin failed (%u)", static_cast<unsigned>(Update.getError()));
    return Result::Rejected;
  }
  size_ = size;
  written_ = 0;
  open_ = true;
  return Result::Ok;
}

Result Esp8266FirmwareWriter::write(const uint8_t* p, size_t n) {
  if (!open_) return Result::NotReady;
  if (size_ && written_ + n > size_) return Result::TooLarge;
  if (Update.write(const_cast<uint8_t*>(p), n) != n) {
    VL_LOGW("ota: flash write failed (%u)", static_cast<unsigned>(Update.getError()));
    return Result::Rejected;
  }
  written_ += n;
  return Result::Ok;
}

Result Esp8266FirmwareWriter::finish() {
  if (!open_) return Result::NotReady;
  if (size_ && written_ != size_) {
    abort();
    return Result::Rejected;
  }
  open_ = false;
  if (!Update.end(true)) {  // trims the reservation to what was written
    VL_LOGW("ota: image refused (%u)", static_cast<unsigned>(Update.getError()));
    return Result::Rejected;
  }
  return Result::Ok;
}

void Esp8266FirmwareWriter::abort() {
  if (!open_) return;
  open_ = false;
  Update.end(false);
}

void Esp8266FirmwareWriter::restart() { ESP.restart(); }

IFirmwareWriter* platformFirmwareWriter() {
  static Esp8266FirmwareWriter writer;
  return &writer;
}

}  // namespace vl

#endif  // VL_PLATFORM_ESP8266
