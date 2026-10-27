#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vl/Config.h"
#include "vl/pal/Clock.h"
#include "vl/pal/Firmware.h"
#include "vl/transport/StreamMqtt.h"

namespace vl {

// An http:// or https:// URL split for a request. host is copied; path points
// into the parsed string ("/" when the URL has none).
struct HttpUrl {
  bool tls = false;
  bool explicitPort = false;  // the URL named a port, so the Host header carries it
  uint16_t port = 0;
  char host[64] = {};
  const char* path = nullptr;
};

// False for another scheme, user info, an empty or over-long host, or a bad port.
bool parseHttpUrl(const char* url, HttpUrl& out);

// IHttpSource for fw_url downloads over INetStreams, so any Arduino Client
// works (WiFiClient, EthernetClient, TinyGsmClient through ArduinoStream).
// Sends HTTP/1.0, so the body is never chunked; resumes with Range; follows
// up to 3 redirects. https:// URLs go through secure, which gets the
// ITlsProvider and must accept it (validate()); without secure they are
// refused. open() waits at most VL_OTA_HTTP_TIMEOUT_MS for each reply's
// headers, and read() fails after that long without a byte; both poll the
// stream and block meanwhile. Used on the client task only.
class StreamHttp : public IHttpSource {
 public:
  // clock null: the platform clock. The streams must outlive this object.
  explicit StreamHttp(INetStream& plain, INetStream* secure = nullptr, IClock* clock = nullptr);
  ~StreamHttp() override { close(); }
  StreamHttp(const StreamHttp&) = delete;
  StreamHttp& operator=(const StreamHttp&) = delete;

  Result open(const char* url, size_t offset, const ITlsProvider* tls, size_t& start, size_t& total) override;
  int read(uint8_t* buf, size_t cap) override;
  void close() override;

 private:
  static const int kMaxRedirects = 3;

  // Connects and writes the GET; stream_ is set even when it fails.
  Result request(const HttpUrl& u, size_t offset, const ITlsProvider* tls);
  // Status line and headers; relocated when a Location was stored in url_.
  Result readHeaders(const HttpUrl& u, int& status, bool& relocated);
  // The trimmed value when line_ is the header name (lower case), else null.
  const char* headerValue(const char* name);
  // Resolves a Location against u into url_; false if it does not fit.
  bool relocate(const HttpUrl& u, const char* loc);
  // A header line without its line ending: its length, or -1 on a timeout or
  // a closed stream. Characters beyond the buffer are dropped and flagged.
  int readLine(bool& truncated);
  bool sendStr(const char* s);
  void stopStream();

  INetStream& plain_;
  INetStream* secure_;
  IClock* clock_;
  INetStream* stream_ = nullptr;  // the open connection
  uint64_t deadline_ = 0;
  bool timedOut_ = false;
  bool sized_ = false;    // Content-Length known
  size_t remaining_ = 0;  // body bytes still to read when sized_
  size_t length_ = 0;     // Content-Length of the current reply
  char url_[VL_OTA_URL_SIZE] = {};
  char line_[VL_OTA_URL_SIZE + 16] = {};  // fits "Location: " and the longest URL
};

}  // namespace vl
