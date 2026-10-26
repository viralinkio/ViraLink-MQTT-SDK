#include "vl/transport/AteccTlsStream.h"

#if VL_HAVE_ATECC_TLS

#include <Arduino.h>
#include <mbedtls/net_sockets.h>
#include <string.h>

#include "vl/Log.h"
#include "vl/core/CertTime.h"
#include "vl/pal/Platform.h"
#include "vl/se/Atecc608.h"

namespace vl {

namespace {

// ECDHE only (forward secrecy), AEAD first. Suites the build lacks are skipped.
const int kSuites[] = {
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,       MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,       MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    0,
};

int rngCb(void*, unsigned char* out, size_t n) {
  while (n) {
    uint32_t r = platformRandom();
    size_t k = n < sizeof(r) ? n : sizeof(r);
    memcpy(out, &r, k);
    out += k;
    n -= k;
  }
  return 0;
}

UtcDate utc(const mbedtls_x509_time& t) {
  UtcDate d = {t.year, t.mon, t.day, t.hour, t.min, t.sec};
  return d;
}

}  // namespace

AteccTlsStream::AteccTlsStream(::Client& client, II2c& bus, IClock* clock)
    : client_(client), bus_(bus), clock_(clock ? clock : &platformClock()) {}

AteccTlsStream::~AteccTlsStream() { stop(); }

Result AteccTlsStream::validate(const ITlsProvider* tls) const {
  if (!tls || !tls->secureElement()) {
    VL_LOGE("atecc-tls: Config::tls must be a SecureElementTls");
    return Result::Rejected;
  }
  if (!tls->caPem() || !tls->clientCertPem()) {
    VL_LOGE("atecc-tls: the CA and the client certificate (PEM) are both required");
    return Result::Rejected;
  }
  return Result::Ok;
}

bool AteccTlsStream::fail(const char* what, int rc) {
  VL_LOGE("atecc-tls: %s failed (-0x%04X)", what, static_cast<unsigned>(-rc));
  stop();
  return false;
}

bool AteccTlsStream::connect(const char* host, uint16_t port, const ITlsProvider* tls) {
  stop();
  if (validate(tls) != Result::Ok) return false;
  address_ = tls->secureElement()->i2cAddress;
  slot_ = tls->secureElement()->keySlot;

  mbedtls_ssl_init(&ssl_);
  mbedtls_ssl_config_init(&conf_);
  mbedtls_x509_crt_init(&ca_);
  mbedtls_x509_crt_init(&cert_);
  mbedtls_pk_init(&key_);
  mbedtls_ecp_keypair_init(&chipKey_.ec);
  chipKey_.self = this;
  open_ = true;

  // PEM parsing needs the terminating NUL counted.
  int rc = mbedtls_x509_crt_parse(&ca_, reinterpret_cast<const unsigned char*>(tls->caPem()), strlen(tls->caPem()) + 1);
  if (rc != 0) return fail("parsing the CA", rc);
  rc = mbedtls_x509_crt_parse(&cert_, reinterpret_cast<const unsigned char*>(tls->clientCertPem()),
                              strlen(tls->clientCertPem()) + 1);
  if (rc != 0) return fail("parsing the client certificate", rc);
  if (!loadKey()) {
    stop();
    return false;
  }

  if ((rc = mbedtls_ssl_config_defaults(&conf_, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
    return fail("TLS setup", rc);
  }
  mbedtls_ssl_conf_min_version(&conf_, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
  mbedtls_ssl_conf_ciphersuites(&conf_, kSuites);
  mbedtls_ssl_conf_authmode(&conf_, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_ca_chain(&conf_, &ca_, nullptr);
  mbedtls_ssl_conf_verify(&conf_, verifyCb, this);
  mbedtls_ssl_conf_rng(&conf_, rngCb, nullptr);
  if ((rc = mbedtls_ssl_conf_own_cert(&conf_, &cert_, &key_)) != 0) return fail("TLS setup", rc);
  if ((rc = mbedtls_ssl_setup(&ssl_, &conf_)) != 0) return fail("TLS setup", rc);
  if ((rc = mbedtls_ssl_set_hostname(&ssl_, host)) != 0) return fail("TLS setup", rc);
  mbedtls_ssl_set_bio(&ssl_, this, sendCb, recvCb, nullptr);

  if (client_.connect(host, port) <= 0) {
    VL_LOGW("atecc-tls: TCP connect to %s:%u failed", host, port);
    stop();
    return false;
  }
  uint64_t start = clock_->uptimeMs();
  while ((rc = mbedtls_ssl_handshake(&ssl_)) != 0) {
    if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
      uint32_t flags = mbedtls_ssl_get_verify_result(&ssl_);
      if (flags && flags != static_cast<uint32_t>(-1)) VL_LOGE("atecc-tls: server certificate refused (flags 0x%x)",
                                                             static_cast<unsigned>(flags));
      return fail("handshake", rc);
    }
    if (clock_->uptimeMs() - start > VL_TLS_TIMEOUT_MS) return fail("handshake (timeout)", rc);
    delay(1);
  }
  handshaken_ = true;
  VL_LOGI("atecc-tls: connected with %s", mbedtls_ssl_get_ciphersuite(&ssl_));
  return true;
}

// Builds the pk context: mbedTLS's EC key with signing replaced by the chip,
// holding the chip's public key, which must be the certificate's.
bool AteccTlsStream::loadKey() {
  Atecc608 chip(bus_, address_);
  uint8_t point[65];
  point[0] = 0x04;  // uncompressed
  if (chip.publicKey(slot_, point + 1) != Result::Ok) {
    VL_LOGE("atecc-tls: no P-256 key read from slot %u of the ATECC608 at 0x%02X", slot_, address_);
    return false;
  }
  mbedtls_ecp_keypair* ec = &chipKey_.ec;
  if (mbedtls_ecp_group_load(&ec->grp, MBEDTLS_ECP_DP_SECP256R1) != 0 ||
      mbedtls_ecp_point_read_binary(&ec->grp, &ec->Q, point, sizeof(point)) != 0) {
    VL_LOGE("atecc-tls: the chip's public key is not a P-256 point");
    return false;
  }
  if (mbedtls_pk_get_type(&cert_.pk) != MBEDTLS_PK_ECKEY ||
      mbedtls_pk_ec(cert_.pk)->grp.id != MBEDTLS_ECP_DP_SECP256R1 ||
      mbedtls_ecp_point_cmp(&mbedtls_pk_ec(cert_.pk)->Q, &ec->Q) != 0) {
    VL_LOGE("atecc-tls: the client certificate is not for the key in slot %u", slot_);
    return false;
  }
  keyInfo_ = *mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY);
  keyInfo_.sign_func = signCb;
  keyInfo_.ctx_free_func = freeKeyCb;
  key_.pk_info = &keyInfo_;
  key_.pk_ctx = &chipKey_;
  return true;
}

int AteccTlsStream::signCb(void* ctx, mbedtls_md_type_t, const unsigned char* hash, size_t hashLen,
                           unsigned char* sig, size_t* sigLen, int (*)(void*, unsigned char*, size_t), void*) {
  AteccTlsStream* self = static_cast<ChipKey*>(ctx)->self;
  // ECDSA on P-256 signs the leftmost 256 bits of the hash; a shorter hash is
  // the same integer zero-extended on the left.
  uint8_t digest[32] = {};
  if (hashLen >= sizeof(digest)) {
    memcpy(digest, hash, sizeof(digest));
  } else {
    memcpy(digest + sizeof(digest) - hashLen, hash, hashLen);
  }
  uint8_t rs[64];
  Atecc608 chip(self->bus_, self->address_);
  if (chip.sign(self->slot_, digest, rs) != Result::Ok) return MBEDTLS_ERR_PK_HW_ACCEL_FAILED;
  *sigLen = ecdsaDer(rs, sig, MBEDTLS_ECDSA_MAX_LEN);
  return *sigLen ? 0 : MBEDTLS_ERR_PK_BAD_INPUT_DATA;
}

// The ChipKey is a member, not mbedTLS's allocation: free its contents only.
void AteccTlsStream::freeKeyCb(void* ctx) { mbedtls_ecp_keypair_free(&static_cast<ChipKey*>(ctx)->ec); }

// mbedTLS cannot tell the time on this build: judge every certificate in the
// chain against the client clock instead (see CertTime.h).
int AteccTlsStream::verifyCb(void* ctx, mbedtls_x509_crt* crt, int depth, uint32_t* flags) {
  AteccTlsStream* self = static_cast<AteccTlsStream*>(ctx);
  *flags &= ~static_cast<uint32_t>(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
  uint64_t ms = 0;
  bool haveNow = self->clock_->utcMs(ms);
  uint8_t v = certValidity(utc(crt->valid_from), utc(crt->valid_to), haveNow, static_cast<int64_t>(ms / 1000),
                           static_cast<int64_t>(VL_TIME_FLOOR_S));
  if (v & kCertExpired) *flags |= MBEDTLS_X509_BADCERT_EXPIRED;
  if (v & kCertNotYetValid) *flags |= MBEDTLS_X509_BADCERT_FUTURE;
  if (v) VL_LOGE("atecc-tls: certificate at depth %d is %s", depth, v & kCertExpired ? "expired" : "not yet valid");
  return 0;
}

int AteccTlsStream::sendCb(void* ctx, const unsigned char* p, size_t n) {
  ::Client& c = static_cast<AteccTlsStream*>(ctx)->client_;
  size_t sent = c.write(p, n);
  if (sent > 0) return static_cast<int>(sent);
  return c.connected() ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_CONN_RESET;
}

int AteccTlsStream::recvCb(void* ctx, unsigned char* buf, size_t n) {
  ::Client& c = static_cast<AteccTlsStream*>(ctx)->client_;
  if (c.available() <= 0) return c.connected() ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_CONN_RESET;
  int got = c.read(buf, n);
  return got > 0 ? got : MBEDTLS_ERR_SSL_WANT_READ;
}

int AteccTlsStream::read(uint8_t* buf, size_t n) {
  if (!handshaken_) return -1;
  int got = mbedtls_ssl_read(&ssl_, buf, n);
  if (got > 0) return got;
  if (got == MBEDTLS_ERR_SSL_WANT_READ || got == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
  return -1;  // closed by the server, or failed
}

bool AteccTlsStream::write(const uint8_t* p, size_t n) {
  if (!handshaken_) return false;
  uint64_t start = clock_->uptimeMs();
  while (n) {
    int sent = mbedtls_ssl_write(&ssl_, p, n);
    if (sent > 0) {
      p += sent;
      n -= static_cast<size_t>(sent);
      continue;
    }
    if (sent != MBEDTLS_ERR_SSL_WANT_WRITE && sent != MBEDTLS_ERR_SSL_WANT_READ) return false;
    if (clock_->uptimeMs() - start > VL_TLS_TIMEOUT_MS) return false;
    delay(1);
  }
  return true;
}

void AteccTlsStream::stop() {
  if (!open_) return;
  if (handshaken_) mbedtls_ssl_close_notify(&ssl_);
  client_.stop();
  mbedtls_ssl_free(&ssl_);
  mbedtls_ssl_config_free(&conf_);
  mbedtls_x509_crt_free(&ca_);
  mbedtls_x509_crt_free(&cert_);
  if (key_.pk_info) {
    mbedtls_pk_free(&key_);  // frees chipKey_'s contents through freeKeyCb
  } else {
    mbedtls_ecp_keypair_free(&chipKey_.ec);
  }
  open_ = false;
  handshaken_ = false;
}

}  // namespace vl

#endif  // VL_HAVE_ATECC_TLS
