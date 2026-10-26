#include "vl/se/Atecc608.h"

#include <string.h>

#include "vl/Log.h"

namespace vl {

namespace {

// I2C word addresses
const uint8_t kWordIdle = 0x02;
const uint8_t kWordCommand = 0x03;

const uint8_t kOpRandom = 0x1B;
const uint8_t kOpNonce = 0x16;
const uint8_t kOpSign = 0x41;
const uint8_t kOpGenKey = 0x40;
const uint8_t kNonceToMsgDigest = 0x43;       // pass-through, 32 bytes, into the message digest buffer
const uint8_t kSignExternalMsgDigest = 0xA0;  // external message, from the message digest buffer
const uint8_t kGenKeyPublic = 0x00;           // public key of an existing private key

const uint8_t kStatusEccFault = 0x05;  // retrying may succeed (datasheet)
const uint8_t kStatusAfterWake = 0x11;
const uint8_t kStatusWatchdog = 0xEE;
const uint8_t kStatusCommError = 0xFF;  // the chip received a corrupted command

const int kAttempts = 3;
const uint32_t kWakeDelayUs = 1500;
const uint32_t kPollUs = 2000;
const uint32_t kPollBudgetUs = 2500000;  // covers the slowest clock-divider setting

// Maximum execution times at the default clock divider (cryptoauthlib), so
// a healthy chip answers the first poll.
uint32_t execUs(uint8_t opcode) {
  switch (opcode) {
    case kOpRandom:
      return 23000;
    case kOpNonce:
      return 20000;
    default:
      return 115000;  // Sign, GenKey
  }
}

bool transient(uint8_t status) {
  return status == kStatusEccFault || status == kStatusAfterWake || status == kStatusWatchdog ||
         status == kStatusCommError;
}

size_t derInteger(const uint8_t* v, uint8_t* out) {
  size_t i = 0;
  while (i < 31 && v[i] == 0) ++i;
  size_t pad = v[i] & 0x80 ? 1 : 0;
  out[0] = 0x02;
  out[1] = static_cast<uint8_t>(32 - i + pad);
  out[2] = 0;
  memcpy(out + 2 + pad, v + i, 32 - i);
  return 2 + pad + 32 - i;
}

}  // namespace

uint16_t ateccCrc(const uint8_t* p, size_t n) {
  uint16_t crc = 0;
  for (size_t i = 0; i < n; ++i) {
    for (uint8_t bit = 1; bit; bit = static_cast<uint8_t>(bit << 1)) {
      bool in = (p[i] & bit) != 0;
      bool top = (crc & 0x8000) != 0;
      crc = static_cast<uint16_t>(crc << 1);
      if (in != top) crc ^= 0x8005;
    }
  }
  return crc;
}

size_t ecdsaDer(const uint8_t rs[64], uint8_t* out, size_t cap) {
  uint8_t der[72];
  size_t n = 2;
  n += derInteger(rs, der + n);
  n += derInteger(rs + 32, der + n);
  der[0] = 0x30;
  der[1] = static_cast<uint8_t>(n - 2);
  if (n > cap) return 0;
  memcpy(out, der, n);
  return n;
}

Result Atecc608::sign(uint8_t slot, const uint8_t digest[32], uint8_t sig[64]) {
  return run(kOpSign, slot, digest, sig);
}

Result Atecc608::publicKey(uint8_t slot, uint8_t pub[64]) { return run(kOpGenKey, slot, nullptr, pub); }

// One operation from wake to idle, attempted again from a fresh wake when the
// chip did not answer cleanly.
Result Atecc608::run(uint8_t opcode, uint8_t slot, const uint8_t* digest, uint8_t* out) {
  Result r = Result::Timeout;
  for (int attempt = 0; attempt < kAttempts; ++attempt) {
    if (attempt) idle();
    status_ = 0;
    if (!wake()) continue;
    if (opcode == kOpSign) {
      uint8_t random[32];  // only updates the RNG seed, as cryptoauthlib does before signing
      r = command(kOpRandom, 0, 0, nullptr, 0, random, sizeof(random));
      if (r == Result::Ok) r = command(kOpNonce, kNonceToMsgDigest, 0, digest, 32, nullptr, 0);
      if (r == Result::Ok) r = command(kOpSign, kSignExternalMsgDigest, slot, nullptr, 0, out, 64);
    } else {
      r = command(kOpGenKey, kGenKeyPublic, slot, nullptr, 0, out, 64);
    }
    if (r != Result::Timeout) break;
  }
  idle();
  if (r != Result::Ok) {
    VL_LOGW("atecc608@0x%02X: %s on slot %u failed (%s, status 0x%02X)", address_,
            opcode == kOpSign ? "sign" : "public key", slot, toString(r), status_);
  }
  return r;
}

bool Atecc608::wake() {
  static const uint8_t kAfterWake[4] = {0x04, kStatusAfterWake, 0x33, 0x43};
  bus_.wakePulse();
  bus_.delayUs(kWakeDelayUs);
  uint8_t r[4];
  return bus_.read(address_, r, sizeof(r)) && memcmp(r, kAfterWake, sizeof(r)) == 0;
}

void Atecc608::idle() {
  uint8_t word = kWordIdle;
  bus_.write(address_, &word, 1);
}

// Sends one command and reads its answer: outLen data bytes, or with outLen 0
// a success status. Timeout for anything worth retrying, Rejected for a
// status the chip will repeat.
Result Atecc608::command(uint8_t opcode, uint8_t p1, uint16_t p2, const uint8_t* data, size_t n, uint8_t* out,
                         size_t outLen) {
  uint8_t pkt[8 + 32];
  if (n > 32 || outLen > 64) return Result::Rejected;
  pkt[0] = kWordCommand;
  pkt[1] = static_cast<uint8_t>(7 + n);
  pkt[2] = opcode;
  pkt[3] = p1;
  pkt[4] = static_cast<uint8_t>(p2 & 0xFF);
  pkt[5] = static_cast<uint8_t>(p2 >> 8);
  if (n) memcpy(pkt + 6, data, n);
  uint16_t crc = ateccCrc(pkt + 1, 5 + n);
  pkt[6 + n] = static_cast<uint8_t>(crc & 0xFF);
  pkt[7 + n] = static_cast<uint8_t>(crc >> 8);
  if (!bus_.write(address_, pkt, 8 + n)) return Result::Timeout;

  // The chip NACKs reads until it has finished.
  bus_.delayUs(execUs(opcode));
  uint8_t resp[3 + 64];
  for (uint32_t waited = 0; !bus_.read(address_, resp, 1); waited += kPollUs) {
    if (waited >= kPollBudgetUs) return Result::Timeout;
    bus_.delayUs(kPollUs);
  }
  size_t count = resp[0];
  if (count < 4 || count > sizeof(resp) || !bus_.read(address_, resp + 1, count - 1)) return Result::Timeout;
  crc = ateccCrc(resp, count - 2);
  if (resp[count - 2] != (crc & 0xFF) || resp[count - 1] != (crc >> 8)) return Result::Timeout;

  if (count == outLen + 3 && outLen) {
    memcpy(out, resp + 1, outLen);
    return Result::Ok;
  }
  if (count != 4) return Result::Timeout;
  if (resp[1] == 0 && outLen == 0) return Result::Ok;
  status_ = resp[1];
  return transient(status_) || status_ == 0 ? Result::Timeout : Result::Rejected;
}

}  // namespace vl
