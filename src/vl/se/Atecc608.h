// Minimal ATECC608A/B driver: ECDSA P-256 signing with a key that never
// leaves the chip, and reading that key's public half. Enough for a TLS client
// key; provisioning the chip (configuration, locking, key generation) is done
// beforehand with Microchip's tools.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vl/Result.h"
#include "vl/pal/I2c.h"

namespace vl {

class Atecc608 {
 public:
  // address is 7-bit: 0x60 factory default, 0x35 Trust&GO, 0x36 TrustFLEX.
  explicit Atecc608(II2c& bus, uint8_t address = 0x60) : bus_(bus), address_(address) {}

  // Signs a 32-byte digest with the private key in slot, whose configuration
  // must allow signing external messages; sig is r || s, big-endian.
  // Timeout: the chip did not answer; Rejected: it refused (no such key, slot
  // not usable for signing).
  Result sign(uint8_t slot, const uint8_t digest[32], uint8_t sig[64]);
  // The public key of the private key in slot, x || y.
  Result publicKey(uint8_t slot, uint8_t pub[64]);

  // Status byte of the last failed command, 0 if the chip did not answer.
  uint8_t lastStatus() const { return status_; }

 private:
  Result run(uint8_t opcode, uint8_t slot, const uint8_t* digest, uint8_t* out);
  bool wake();
  void idle();
  Result command(uint8_t opcode, uint8_t p1, uint16_t p2, const uint8_t* data, size_t n, uint8_t* out, size_t outLen);

  II2c& bus_;
  uint8_t address_;
  uint8_t status_ = 0;
};

// CRC-16 of the ATECC protocol: polynomial 0x8005, bits taken LSB first,
// initial value 0. Sent low byte first.
uint16_t ateccCrc(const uint8_t* p, size_t n);

// ASN.1 DER ECDSA-Sig-Value for a raw r || s (P-256). Returns the length, at
// most 72, or 0 if cap is too small.
size_t ecdsaDer(const uint8_t rs[64], uint8_t* out, size_t cap);

}  // namespace vl
