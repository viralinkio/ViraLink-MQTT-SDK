#pragma once

#if defined(ARDUINO)

#include <Client.h>

#include "vl/transport/StreamMqtt.h"

namespace vl {

// INetStream over any Arduino Client: WiFiClient, EthernetClient (W5500),
// TinyGsmClient. A TLS client the application configures itself, such as
// SSLClient, works too; leave Config::tls null then, since the library
// cannot see inside the Client. Pair with BufferedStreamMqtt.
class ArduinoStream : public INetStream {
 public:
  explicit ArduinoStream(::Client& client) : client_(client) {}

  bool connect(const char* host, uint16_t port, const ITlsProvider* tls) override {
    (void)tls;
    return client_.connect(host, port) > 0;
  }
  int read(uint8_t* buf, size_t n) override {
    int waiting = client_.available();
    if (waiting <= 0) return client_.connected() ? 0 : -1;
    if (n > static_cast<size_t>(waiting)) n = static_cast<size_t>(waiting);
    int got = client_.read(buf, n);
    return got > 0 ? got : 0;
  }
  bool write(const uint8_t* p, size_t n) override {
    while (n) {
      size_t sent = client_.write(p, n);
      if (sent == 0 || sent > n) return false;
      p += sent;
      n -= sent;
    }
    return true;
  }
  void stop() override { client_.stop(); }

 private:
  ::Client& client_;  // Arduino's, not vl::Client
};

}  // namespace vl

#endif  // ARDUINO
