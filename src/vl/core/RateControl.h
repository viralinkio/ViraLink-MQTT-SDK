#pragma once

#include <stdint.h>

namespace vl {

// Exponential reconnect backoff with +-20 % jitter so a fleet does not
// reconnect in lockstep after a broker restart.
class Backoff {
 public:
  Backoff(uint32_t baseMs = 1000, uint32_t capMs = 60000) : base_(baseMs), cap_(capMs) {}

  void seed(uint32_t s) { rng_ = s ? s : 0x9E3779B9u; }
  void reset() { attempt_ = 0; }
  uint8_t attempt() const { return attempt_; }

  // Delay before the next attempt; never below minMs.
  uint32_t next(uint32_t minMs = 0) {
    uint32_t d = attempt_ >= 31 ? cap_ : base_ << attempt_;
    if (d > cap_ || d < base_) d = cap_;
    if (attempt_ < 31) ++attempt_;
    uint32_t pct = 80u + rand() % 41u;  // 80..120
    d = static_cast<uint32_t>(static_cast<uint64_t>(d) * pct / 100u);
    return d < minMs ? minMs : d;
  }

 private:
  uint32_t rand() {  // xorshift32
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
  }

  uint32_t base_;
  uint32_t cap_;
  uint32_t rng_ = 0x9E3779B9u;
  uint8_t attempt_ = 0;
};

// Token bucket for the outbox drain; matches tenant-profile and gateway
// rate limits. ratePerSec == 0 disables limiting.
class TokenBucket {
 public:
  void configure(uint16_t ratePerSec, uint16_t burst) {
    rate_ = ratePerSec;
    burst_ = burst ? burst : 1;
    milliTokens_ = static_cast<uint32_t>(burst_) * 1000u;
  }

  bool tryTake(uint64_t nowMs) {
    if (rate_ == 0) return true;
    if (last_ == 0 || nowMs < last_) last_ = nowMs;
    uint64_t add = (nowMs - last_) * rate_;
    last_ = nowMs;
    uint64_t cap = static_cast<uint64_t>(burst_) * 1000u;
    uint64_t t = milliTokens_ + add;
    milliTokens_ = static_cast<uint32_t>(t > cap ? cap : t);
    if (milliTokens_ < 1000u) return false;
    milliTokens_ -= 1000u;
    return true;
  }

 private:
  uint64_t last_ = 0;
  uint32_t milliTokens_ = 0;
  uint16_t rate_ = 0;
  uint16_t burst_ = 1;
};

}  // namespace vl
