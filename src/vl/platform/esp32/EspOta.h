#pragma once

#include "vl/Config.h"

#if VL_PLATFORM_ESP32

#include <stddef.h>
#include <stdint.h>

#include "vl/pal/Firmware.h"

namespace vl {

// IFirmwareWriter over esp_ota_ops, for Arduino and ESP-IDF. Writes the next
// OTA app partition, erasing sector by sector as it goes. Confirmation and
// rollback need CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE (ESP-IDF menuconfig);
// without it a new image is valid as soon as it boots.
class EspFirmwareWriter : public IFirmwareWriter {
 public:
  Result begin(size_t size) override;
  Result write(const uint8_t* p, size_t n) override;
  Result finish() override;
  void abort() override;
  void restart() override;
  bool pendingConfirm() override;
  void confirm() override;
  void rollback() override;
  bool rolledBack() override;

 private:
  const void* part_ = nullptr;  // const esp_partition_t*
  uint32_t handle_ = 0;         // esp_ota_handle_t
  bool open_ = false;
};

// IHttpSource over esp_http_client. Follows up to 3 redirects. HTTPS uses the
// CA from the ITlsProvider, or the ESP-IDF certificate bundle when there is
// none (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE). esp_http_client allocates its
// buffers while a download runs.
class EspHttpSource : public IHttpSource {
 public:
  ~EspHttpSource() override { close(); }
  Result open(const char* url, size_t offset, const ITlsProvider* tls, size_t& start, size_t& total) override;
  int read(uint8_t* buf, size_t cap) override;
  void close() override;

 private:
  void* client_ = nullptr;  // esp_http_client_handle_t
};

}  // namespace vl

#endif  // VL_PLATFORM_ESP32
