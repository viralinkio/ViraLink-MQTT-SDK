#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vl/Result.h"
#include "vl/transport/IMqttTransport.h"

namespace vl {

// Writes a firmware image into the inactive slot and selects what boots
// next. Used on the client task only.
class IFirmwareWriter {
 public:
  virtual ~IFirmwareWriter() {}
  // Prepares the inactive slot for size bytes (0 = unknown). TooLarge if the
  // slot is smaller.
  virtual Result begin(size_t size) = 0;
  virtual Result write(const uint8_t* p, size_t n) = 0;
  // Checks the complete image and boots it on the next restart.
  virtual Result finish() = 0;
  // Drops a partly written image. Safe to call when nothing is open.
  virtual void abort() = 0;
  // Does not return on hardware.
  virtual void restart() = 0;

  // Rollback; the defaults suit a platform without it.
  // True while the running image is new and not yet confirmed.
  virtual bool pendingConfirm() { return false; }
  // Keeps the running image, cancelling the automatic rollback.
  virtual void confirm() {}
  // Marks the running image bad and restarts into the previous one.
  virtual void rollback() {}
  // True if a newer image was rolled back and this one is running instead.
  virtual bool rolledBack() { return false; }
};

// Streams a file over HTTP or HTTPS for fw_url packages. Used on the client
// task only.
class IHttpSource {
 public:
  virtual ~IHttpSource() {}
  // Opens url at byte offset (a Range request when offset > 0). On Ok, start
  // is where the body really begins (0 if the server ignored the Range) and
  // total is the file size, 0 if unknown.
  virtual Result open(const char* url, size_t offset, const ITlsProvider* tls, size_t& start, size_t& total) = 0;
  // Up to cap bytes: the count read, 0 at the end of the body, -1 on an error
  // or a stall longer than VL_OTA_HTTP_TIMEOUT_MS.
  virtual int read(uint8_t* buf, size_t cap) = 0;
  virtual void close() = 0;
};

// The platform's implementations, or null where there is none: set one with
// Ota::setWriter() / Ota::setHttp(). ESP32 has both; ESP8266 has the writer,
// and downloads go through a StreamHttp the application sets up.
IFirmwareWriter* platformFirmwareWriter();
IHttpSource* platformHttpSource();

}  // namespace vl
