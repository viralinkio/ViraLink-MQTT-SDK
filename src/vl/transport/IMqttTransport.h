#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vl {

// Trust anchors and client identity for TLS. Pin the CA, never the server
// leaf certificate: ThingsBoard 4.2.2.2+ rotates server certificates live.
class ITlsProvider {
 public:
  virtual ~ITlsProvider() {}
  virtual const char* caPem() const = 0;
  virtual const char* clientCertPem() const { return nullptr; }  // mTLS only
  virtual const char* clientKeyPem() const { return nullptr; }   // null when the key lives in a secure element
};

// PEM strings held by the application (must outlive the client).
class PemTls : public ITlsProvider {
 public:
  explicit PemTls(const char* ca, const char* cert = nullptr, const char* key = nullptr)
      : ca_(ca), cert_(cert), key_(key) {}
  const char* caPem() const override { return ca_; }
  const char* clientCertPem() const override { return cert_; }
  const char* clientKeyPem() const override { return key_; }

 private:
  const char* ca_;
  const char* cert_;
  const char* key_;
};

struct ConnectOptions {
  const char* host = nullptr;
  uint16_t port = 1883;
  const char* clientId = nullptr;  // <= 23 characters
  const char* username = nullptr;
  const char* password = nullptr;
  uint16_t keepAliveS = 60;
  const ITlsProvider* tls = nullptr;  // null = plain TCP
};

// MQTT 3.1.1 transport. All callbacks run on the client task (inside loop()),
// never on a transport-internal task; ctx is passed back unchanged.
class IMqttTransport {
 public:
  typedef void (*ConnackCb)(uint8_t returnCode, void* ctx);
  typedef void (*SubackCb)(const char* filter, bool granted, void* ctx);
  typedef void (*PubackCb)(uint16_t packetId, void* ctx);
  typedef void (*MessageCb)(const char* topic, const uint8_t* p, size_t n, void* ctx);
  typedef void (*ClosedCb)(void* ctx);

  virtual ~IMqttTransport() {}

  virtual void beginConnect(const ConnectOptions& opts) = 0;  // non-blocking; result via onConnack
  virtual void disconnect() = 0;
  virtual bool connected() const = 0;
  // false = not sent (retry later). For qos 1, *packetId receives the id the
  // PUBACK will carry.
  virtual bool publish(const char* topic, const uint8_t* p, size_t n, uint8_t qos, uint16_t* packetId) = 0;
  virtual void subscribe(const char* filter, uint8_t qos) = 0;  // result via onSuback
  virtual void loop() = 0;
  virtual size_t maxPacket() const = 0;

  virtual void onConnack(ConnackCb cb, void* ctx) = 0;
  virtual void onSuback(SubackCb cb, void* ctx) = 0;
  virtual void onPuback(PubackCb cb, void* ctx) = 0;
  virtual void onMessage(MessageCb cb, void* ctx) = 0;
  virtual void onClosed(ClosedCb cb, void* ctx) = 0;
};

}  // namespace vl
