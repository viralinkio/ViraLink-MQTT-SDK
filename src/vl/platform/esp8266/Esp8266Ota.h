#pragma once

#include "vl/Config.h"

#if VL_PLATFORM_ESP8266

#include <stddef.h>
#include <stdint.h>

#include "vl/pal/Firmware.h"

namespace vl {

// IFirmwareWriter over the ESP8266 core's Update: the image goes into the
// free flash after the running sketch and eboot copies it over on the next
// restart. There is no rollback, so the IFirmwareWriter defaults apply. Signed
// updates (Update.installSignature()) still verify in finish().
class Esp8266FirmwareWriter : public IFirmwareWriter {
 public:
  Result begin(size_t size) override;
  Result write(const uint8_t* p, size_t n) override;
  Result finish() override;
  void abort() override;
  void restart() override;

 private:
  size_t size_ = 0;  // 0 = unknown
  size_t written_ = 0;
  bool open_ = false;
};

}  // namespace vl

#endif  // VL_PLATFORM_ESP8266
