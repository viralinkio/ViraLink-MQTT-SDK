#include "vl/Config.h"

#if VL_PLATFORM_ESP8266

#include <Arduino.h>
#include <ESP8266HTTPClient.h>
#include <Updater.h>
#include <WiFiClient.h>
#include <WiFiClientSecureBearSSL.h>

#include <new>

#include "vl/Log.h"
#include "vl/pal/Firmware.h"

namespace vl {

namespace {

const char kAnyFailingMd5[] = "00000000000000000000000000000000";

// Host and port of an https:// URL; false if the host does not fit.
bool httpsHost(const char* url, char* host, size_t cap, uint16_t& port) {
  const char* p = url + 8;
  size_t n = strcspn(p, ":/?");
  if (n == 0 || n >= cap) return false;
  memcpy(host, p, n);
  host[n] = '\0';
  port = p[n] == ':' ? static_cast<uint16_t>(atoi(p + n + 1)) : 443;
  return port != 0;
}

// IFirmwareWriter over the core's Updater, which stages the image for eboot
// to copy over the running one on the next restart. No rollback.
class Esp8266FirmwareWriter : public IFirmwareWriter {
 public:
  Result begin(size_t size) override {
    abort();
    size_t space = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if (size > space) return Result::TooLarge;
    // One byte more than the image, so Updater never finds it complete by
    // itself: abort() after the last byte (a checksum mismatch) then resets
    // it instead of staging it. finish() commits what was written.
    size_t room = size && size < space ? size + 1 : space;
    if (!Update.begin(room)) {
      VL_LOGW("ota: Updater refused %lu bytes, error %u", static_cast<unsigned long>(room),
              static_cast<unsigned>(Update.getError()));
      return Update.getError() == UPDATE_ERROR_SPACE ? Result::TooLarge : Result::NotReady;
    }
    open_ = true;
    return Result::Ok;
  }
  Result write(const uint8_t* p, size_t n) override {
    if (!open_) return Result::NotReady;
    // Updater copies from p; it only lacks the const.
    return Update.write(const_cast<uint8_t*>(p), n) == n ? Result::Ok : Result::Rejected;
  }
  Result finish() override {
    if (!open_) return Result::NotReady;
    open_ = false;
    if (Update.end(true)) return Result::Ok;
    VL_LOGW("ota: Updater rejected the image, error %u", static_cast<unsigned>(Update.getError()));
    return Result::Rejected;
  }
  void abort() override {
    if (!open_) return;
    open_ = false;
    // Only a complete image would be staged by end(); a failing MD5 stops it,
    // and the second end() resets the error state.
    if (Update.isFinished()) Update.setMD5(kAnyFailingMd5);
    Update.end();
    if (Update.isRunning()) Update.end();
  }
  void restart() override { ESP.restart(); }

 private:
  bool open_ = false;
};

// IHttpSource over ESP8266HTTPClient. HTTPS trusts the ITlsProvider's CA and
// is refused without one: the core has no certificate bundle. BearSSL takes
// its buffers from the heap while a download runs: about 2 KB when the server
// accepts a 1 KB maximum fragment length (MFLN), else about 17 KB, which a
// busy sketch may not have. Chunked responses are
// refused; a fw_url package should come from a plain file server.
class Esp8266HttpSource : public IHttpSource {
 public:
  ~Esp8266HttpSource() override {
    close();
    dropCa();
  }

  Result open(const char* url, size_t offset, const ITlsProvider* tls, size_t& start, size_t& total) override {
    close();
    bool https = strncmp(url, "https://", 8) == 0;
    WiFiClient* net = &plain_;
    if (https) {
      if (!tls || !tls->caPem()) {
        VL_LOGW("ota: HTTPS needs a CA (Ota::setHttpTls or Config::tls)");
        return Result::Rejected;
      }
      if (!trust(tls)) return Result::Rejected;
      char host[64];
      uint16_t port = 0;
      if (!httpsHost(url, host, sizeof(host), port)) return Result::Rejected;
      bool mfln = BearSSL::WiFiClientSecure::probeMaxFragmentLength(host, port, 1024);
      secure_.setBufferSizes(mfln ? 1024 : 16384, mfln ? 1024 : 512);
      net = &secure_;
    }
    http_.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http_.setTimeout(VL_OTA_HTTP_TIMEOUT_MS);
    if (!http_.begin(*net, url)) return Result::Rejected;
    open_ = true;
    if (offset) {
      char range[32];
      snprintf(range, sizeof(range), "bytes=%lu-", static_cast<unsigned long>(offset));
      http_.addHeader("Range", range);
    }
    const char* headers[] = {"Transfer-Encoding"};  // collectHeaders() takes a non-const array
    http_.collectHeaders(headers, 1);
    int status = http_.GET();
    if (status != 200 && status != 206) {
      VL_LOGW("ota: HTTP status %d", status);
      close();
      return Result::Rejected;
    }
    if (http_.header("Transfer-Encoding").equalsIgnoreCase("chunked")) {
      VL_LOGW("ota: chunked HTTP responses are not supported");
      close();
      return Result::Rejected;
    }
    int len = http_.getSize();
    start = status == 206 ? offset : 0;
    total = len > 0 ? start + static_cast<size_t>(len) : 0;
    pos_ = start;
    total_ = total;
    return Result::Ok;
  }

  int read(uint8_t* buf, size_t cap) override {
    WiFiClient* s = open_ ? http_.getStreamPtr() : nullptr;
    if (!s) return -1;
    if (total_ && pos_ >= total_) return 0;
    if (total_ && cap > total_ - pos_) cap = total_ - pos_;
    uint32_t since = millis();
    for (;;) {
      int avail = s->available();
      if (avail > 0) {
        size_t n = cap < static_cast<size_t>(avail) ? cap : static_cast<size_t>(avail);
        int got = s->read(buf, n);
        if (got > 0) {
          pos_ += static_cast<size_t>(got);
          return got;
        }
      } else if (!s->connected()) {
        return total_ ? -1 : 0;  // without a length, the close ends the body
      }
      if (millis() - since > VL_OTA_HTTP_TIMEOUT_MS) return -1;
      delay(1);  // lets the WiFi stack run
    }
  }

  void close() override {
    if (!open_) return;
    open_ = false;
    http_.end();
  }

 private:
  // X509List cannot be cleared, so a different provider rebuilds it in place.
  bool trust(const ITlsProvider* tls) {
    if (caFrom_ == tls) return true;
    dropCa();
    BearSSL::X509List* ca = new (caStore_) BearSSL::X509List(tls->caPem());
    caFrom_ = tls;
    if (ca->getCount() == 0) {
      VL_LOGW("ota: the CA PEM did not parse");
      dropCa();
      return false;
    }
    secure_.setTrustAnchors(ca);
    return true;
  }
  void dropCa() {
    if (!caFrom_) return;
    reinterpret_cast<BearSSL::X509List*>(caStore_)->~X509List();
    caFrom_ = nullptr;
  }

  HTTPClient http_;
  WiFiClient plain_;
  BearSSL::WiFiClientSecure secure_;
  alignas(BearSSL::X509List) unsigned char caStore_[sizeof(BearSSL::X509List)];
  const ITlsProvider* caFrom_ = nullptr;
  bool open_ = false;
  size_t pos_ = 0;
  size_t total_ = 0;
};

}  // namespace

IFirmwareWriter* platformFirmwareWriter() {
  static Esp8266FirmwareWriter writer;
  return &writer;
}

IHttpSource* platformHttpSource() {
  static Esp8266HttpSource source;
  return &source;
}

}  // namespace vl

#endif  // VL_PLATFORM_ESP8266
