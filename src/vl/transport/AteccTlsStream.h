// mTLS with the client key in an ATECC608A/B, for Arduino cores that ship
// mbedTLS 2.x (Arduino-ESP32 2.0.x). VL_HAVE_ATECC_TLS says whether this build
// has it; ESP-IDF builds use EspMqtt with esp-tls instead.
#pragma once

#include "vl/Config.h"

#if defined(ARDUINO) && VL_PLATFORM_ESP32 && defined(__has_include)
#if __has_include(<Client.h>) && __has_include(<mbedtls/pk_internal.h>)  // pk_internal.h: mbedTLS 2.x only
#define VL_HAVE_ATECC_TLS 1
#endif
#endif
#ifndef VL_HAVE_ATECC_TLS
#define VL_HAVE_ATECC_TLS 0
#endif

#if VL_HAVE_ATECC_TLS

#include <Client.h>
#include <mbedtls/pk.h>
#include <mbedtls/pk_internal.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "vl/pal/Clock.h"
#include "vl/pal/I2c.h"
#include "vl/transport/StreamMqtt.h"

namespace vl {

// TLS 1.2 over any Arduino Client (WiFiClient, EthernetClient, TinyGsmClient)
// with the client key in an ATECC608A/B: mbedTLS runs the handshake and the
// chip makes the one signature mTLS needs. Config::tls must be a
// SecureElementTls with the CA and the client certificate (PEM); connect()
// refuses a certificate whose public key is not the chip's.
//
// Certificate dates are checked against the client clock: both dates once
// the wall clock is known, before that only expiry before VL_TIME_FLOOR_S.
// connect() blocks for the TCP connect and the handshake (at most
// VL_TLS_TIMEOUT_MS). mbedTLS allocates the session (about 40 KB) at connect
// and frees it at stop(), as esp-mqtt does.
class AteccTlsStream : public INetStream {
 public:
  // clock null: the platform clock.
  AteccTlsStream(::Client& client, II2c& bus, IClock* clock = nullptr);
  ~AteccTlsStream() override;
  AteccTlsStream(const AteccTlsStream&) = delete;
  AteccTlsStream& operator=(const AteccTlsStream&) = delete;

  Result validate(const ITlsProvider* tls) const override;
  bool connect(const char* host, uint16_t port, const ITlsProvider* tls) override;
  int read(uint8_t* buf, size_t n) override;
  bool write(const uint8_t* p, size_t n) override;
  void stop() override;

 private:
  // pk context of the chip's key: an EC key (public half loaded) whose
  // signatures come from the chip. ec first, so mbedTLS can treat a pointer
  // to this as an mbedtls_ecp_keypair.
  struct ChipKey {
    mbedtls_ecp_keypair ec;
    AteccTlsStream* self;
  };

  bool loadKey();
  bool fail(const char* what, int rc);
  static int sendCb(void* ctx, const unsigned char* p, size_t n);
  static int recvCb(void* ctx, unsigned char* buf, size_t n);
  static int verifyCb(void* ctx, mbedtls_x509_crt* crt, int depth, uint32_t* flags);
  static int signCb(void* ctx, mbedtls_md_type_t md, const unsigned char* hash, size_t hashLen, unsigned char* sig,
                    size_t* sigLen, int (*rng)(void*, unsigned char*, size_t), void* rngCtx);
  static void freeKeyCb(void* ctx);

  ::Client& client_;  // Arduino's, not vl::Client
  II2c& bus_;
  IClock* clock_;
  bool open_ = false;       // contexts initialised
  bool handshaken_ = false;
  uint8_t address_ = 0;
  uint8_t slot_ = 0;
  mbedtls_ssl_context ssl_;
  mbedtls_ssl_config conf_;
  mbedtls_x509_crt ca_;
  mbedtls_x509_crt cert_;
  mbedtls_pk_context key_;
  mbedtls_pk_info_t keyInfo_;
  ChipKey chipKey_;
};

}  // namespace vl

#endif  // VL_HAVE_ATECC_TLS
