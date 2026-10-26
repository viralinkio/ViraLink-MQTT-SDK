// ATECC608 driver against a simulated chip: golden command bytes, the sign
// and public-key flows, busy polling, wake and transient-error retries, and
// the DER signature encoding. Golden CRCs come from an independent Python
// implementation that reproduces the datasheet's wake response 04 11 33 43.
#include <string.h>
#include <unity.h>

#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "vl/se/Atecc608.h"

using namespace vl;

void setUp() {}
void tearDown() {}

namespace {

typedef std::vector<uint8_t> Buf;

Buf withCrc(Buf b) {
  uint16_t c = ateccCrc(b.data(), b.size());
  b.push_back(static_cast<uint8_t>(c & 0xFF));
  b.push_back(static_cast<uint8_t>(c >> 8));
  return b;
}

// The chip as its I2C interface shows it: asleep, idle or awake; NACKs while
// asleep or busy; answers Random, Nonce, Sign and GenKey.
struct FakeChip : II2c {
  enum State { Asleep, Idle, Awake };
  uint8_t address = 0x60;
  State state = Asleep;
  Buf out;  // response being read
  size_t outPos = 0;
  int busyReads = 0;     // reads NACKed after each command
  int busyNow = 0;
  int wakeFailures = 0;  // wake pulses ignored
  int corruptNext = 0;   // responses sent with a bad CRC
  std::deque<std::pair<uint8_t, uint8_t> > statuses;  // {opcode, status}: the next such command answers status
  std::vector<std::string> log;
  std::vector<Buf> packets;  // commands as written, word address included
  uint64_t delayedUs = 0;
  Buf msgDigest;  // message digest buffer; empty = invalid
  uint8_t sig[64];
  uint8_t pub[64];

  FakeChip() {
    for (int i = 0; i < 64; ++i) {
      sig[i] = static_cast<uint8_t>(0xA0 + i);
      pub[i] = static_cast<uint8_t>(0x10 + i);
    }
  }

  bool write(uint8_t addr, const uint8_t* p, size_t n) override {
    if (addr != address || state == Asleep || n == 0) return false;
    if (p[0] == 0x02) {
      log.push_back("idle");
      state = Idle;
      return true;
    }
    if (p[0] != 0x03 || state != Awake) return false;
    packets.push_back(Buf(p, p + n));
    execute(Buf(p + 1, p + n));
    busyNow = busyReads;
    return true;
  }
  bool read(uint8_t addr, uint8_t* buf, size_t n) override {
    if (addr != address || state == Asleep) return false;
    if (busyNow > 0) {
      --busyNow;
      return false;
    }
    for (size_t i = 0; i < n; ++i) buf[i] = outPos < out.size() ? out[outPos++] : 0xFF;
    return true;
  }
  void wakePulse() override {
    if (wakeFailures > 0) {
      --wakeFailures;
      return;
    }
    if (state == Awake) return;  // still awake: the output buffer keeps the last response
    log.push_back("wake");
    state = Awake;
    busyNow = 0;
    respond(Buf{0x04, 0x11, 0x33, 0x43});
  }
  void delayUs(uint32_t us) override { delayedUs += us; }

  void respond(const Buf& b) {
    out = b;
    outPos = 0;
  }
  void status(uint8_t s) { respond(withCrc(Buf{0x04, s})); }
  void data(const uint8_t* p, size_t n) {
    Buf b{static_cast<uint8_t>(n + 3)};
    b.insert(b.end(), p, p + n);
    respond(withCrc(b));
  }

  void execute(const Buf& c) {
    if (c.size() < 7 || c[0] != c.size() || ateccCrc(c.data(), c.size() - 2) != (c[c.size() - 2] | c[c.size() - 1] << 8)) {
      log.push_back("bad packet");
      status(0xFF);
      return;
    }
    uint8_t op = c[1], p1 = c[2];
    uint16_t p2 = static_cast<uint16_t>(c[3] | c[4] << 8);
    if (!statuses.empty() && statuses.front().first == op) {
      log.push_back("op " + std::to_string(op) + " -> status " + std::to_string(statuses.front().second));
      status(statuses.front().second);
      statuses.pop_front();
    } else if (op == 0x1B && p1 == 0x00 && p2 == 0 && c.size() == 7) {
      log.push_back("random");
      uint8_t r[32] = {};
      data(r, sizeof(r));
    } else if (op == 0x16 && p1 == 0x43 && p2 == 0 && c.size() == 7 + 32) {
      log.push_back("nonce");
      msgDigest.assign(c.begin() + 5, c.begin() + 37);
      status(0x00);
    } else if (op == 0x41 && p1 == 0xA0 && c.size() == 7 && msgDigest.size() == 32) {
      log.push_back("sign " + std::to_string(p2));
      msgDigest.clear();
      data(sig, sizeof(sig));
    } else if (op == 0x40 && p1 == 0x00 && c.size() == 7) {
      log.push_back("genkey " + std::to_string(p2));
      data(pub, sizeof(pub));
    } else {
      log.push_back("op " + std::to_string(op) + " refused");
      status(0x0F);
    }
    if (corruptNext > 0) {
      --corruptNext;
      out.back() ^= 0x01;
    }
  }

  std::string events() {
    std::string r;
    for (size_t i = 0; i < log.size(); ++i) r += (i ? " | " : "") + log[i];
    log.clear();
    return r;
  }
};

void assertBytes(const Buf& expected, const uint8_t* got, size_t n) {
  TEST_ASSERT_EQUAL_UINT(expected.size(), n);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected.data(), got, n);
}

uint8_t kDigest[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
                       16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};

}  // namespace

static void test_crc() {
  uint8_t wake[] = {0x04, 0x11};
  TEST_ASSERT_EQUAL_HEX16(0x4333, ateccCrc(wake, 2));
  uint8_t ok[] = {0x04, 0x00};
  TEST_ASSERT_EQUAL_HEX16(0x4003, ateccCrc(ok, 2));
  TEST_ASSERT_EQUAL_HEX16(0x0000, ateccCrc(nullptr, 0));
}

static void test_sign_flow_and_golden_packets() {
  FakeChip chip;
  Atecc608 se(chip);
  uint8_t sig[64];
  TEST_ASSERT_EQUAL(Result::Ok, se.sign(2, kDigest, sig));
  TEST_ASSERT_EQUAL_STRING("wake | random | nonce | sign 2 | idle", chip.events().c_str());
  TEST_ASSERT_EQUAL_HEX8_ARRAY(chip.sig, sig, 64);

  TEST_ASSERT_EQUAL_UINT(3, chip.packets.size());
  const Buf random{0x03, 0x07, 0x1B, 0x00, 0x00, 0x00, 0x24, 0xCD};
  assertBytes(random, chip.packets[0].data(), chip.packets[0].size());
  Buf nonce{0x03, 0x27, 0x16, 0x43, 0x00, 0x00};
  nonce.insert(nonce.end(), kDigest, kDigest + 32);
  nonce.push_back(0xD7);
  nonce.push_back(0x6C);
  assertBytes(nonce, chip.packets[1].data(), chip.packets[1].size());
  const Buf sign{0x03, 0x07, 0x41, 0xA0, 0x02, 0x00, 0x7D, 0x05};
  assertBytes(sign, chip.packets[2].data(), chip.packets[2].size());
  TEST_ASSERT_EQUAL(FakeChip::Idle, chip.state);
}

static void test_public_key() {
  FakeChip chip;
  chip.address = 0x35;
  Atecc608 se(chip, 0x35);
  uint8_t pub[64];
  TEST_ASSERT_EQUAL(Result::Ok, se.publicKey(0, pub));
  TEST_ASSERT_EQUAL_STRING("wake | genkey 0 | idle", chip.events().c_str());
  TEST_ASSERT_EQUAL_HEX8_ARRAY(chip.pub, pub, 64);
  const Buf genkey{0x03, 0x07, 0x40, 0x00, 0x00, 0x00, 0x00, 0x05};
  assertBytes(genkey, chip.packets[0].data(), chip.packets[0].size());
}

static void test_busy_chip_is_polled() {
  FakeChip chip;
  chip.busyReads = 20;  // 40 ms past the maximum execution time
  Atecc608 se(chip);
  uint8_t sig[64];
  TEST_ASSERT_EQUAL(Result::Ok, se.sign(0, kDigest, sig));
  TEST_ASSERT_EQUAL_STRING("wake | random | nonce | sign 0 | idle", chip.events().c_str());
  TEST_ASSERT_EQUAL_HEX8_ARRAY(chip.sig, sig, 64);
}

static void test_wake_retried() {
  FakeChip chip;
  chip.wakeFailures = 2;
  Atecc608 se(chip);
  uint8_t pub[64];
  TEST_ASSERT_EQUAL(Result::Ok, se.publicKey(0, pub));
  TEST_ASSERT_EQUAL_STRING("wake | genkey 0 | idle", chip.events().c_str());

  FakeChip dead;
  dead.wakeFailures = 3;
  Atecc608 se2(dead);
  TEST_ASSERT_EQUAL(Result::Timeout, se2.publicKey(0, pub));
  TEST_ASSERT_EQUAL_UINT(0, dead.packets.size());
  TEST_ASSERT_EQUAL_HEX8(0, se2.lastStatus());
}

static void test_no_chip_at_address() {
  FakeChip chip;
  Atecc608 se(chip, 0x36);
  uint8_t sig[64];
  TEST_ASSERT_EQUAL(Result::Timeout, se.sign(0, kDigest, sig));
  TEST_ASSERT_EQUAL_UINT(0, chip.packets.size());
  TEST_ASSERT_TRUE(chip.delayedUs < 100000);  // gives up without polling
}

static void test_chip_left_awake_recovers() {
  FakeChip chip;
  chip.state = FakeChip::Awake;
  chip.status(0x00);  // stale response from an interrupted operation
  Atecc608 se(chip);
  uint8_t pub[64];
  TEST_ASSERT_EQUAL(Result::Ok, se.publicKey(0, pub));
  TEST_ASSERT_EQUAL_STRING("idle | wake | genkey 0 | idle", chip.events().c_str());
}

static void test_error_status_fails_at_once() {
  FakeChip chip;
  chip.statuses.push_back(std::make_pair(0x40, 0x0F));  // execution error: e.g. the slot holds no private key
  Atecc608 se(chip);
  uint8_t pub[64];
  TEST_ASSERT_EQUAL(Result::Rejected, se.publicKey(9, pub));
  TEST_ASSERT_EQUAL_STRING("wake | op 64 -> status 15 | idle", chip.events().c_str());
  TEST_ASSERT_EQUAL_HEX8(0x0F, se.lastStatus());
}

static void test_transient_statuses_retried() {
  FakeChip chip;
  chip.statuses.push_back(std::make_pair(0x1B, 0xEE));  // Random: watchdog about to expire
  chip.statuses.push_back(std::make_pair(0x16, 0x11));  // Nonce: the chip had gone to sleep
  Atecc608 se(chip);
  uint8_t sig[64];
  TEST_ASSERT_EQUAL(Result::Ok, se.sign(1, kDigest, sig));
  TEST_ASSERT_EQUAL_STRING(
      "wake | op 27 -> status 238 | idle | wake | random | op 22 -> status 17 | idle | wake | random | nonce | sign 1 "
      "| idle",
      chip.events().c_str());

  FakeChip bad;
  bad.statuses.assign(3, std::make_pair(0x40, 0xFF));  // the chip saw a corrupted command
  Atecc608 se2(bad);
  uint8_t pub[64];
  TEST_ASSERT_EQUAL(Result::Timeout, se2.publicKey(0, pub));
  TEST_ASSERT_EQUAL_UINT(3, bad.packets.size());
  TEST_ASSERT_EQUAL_HEX8(0xFF, se2.lastStatus());
}

static void test_bad_crc_retried() {
  FakeChip chip;
  chip.corruptNext = 1;
  Atecc608 se(chip);
  uint8_t pub[64];
  TEST_ASSERT_EQUAL(Result::Ok, se.publicKey(0, pub));
  TEST_ASSERT_EQUAL_STRING("wake | genkey 0 | idle | wake | genkey 0 | idle", chip.events().c_str());
  TEST_ASSERT_EQUAL_HEX8_ARRAY(chip.pub, pub, 64);
}

static void test_chip_that_never_answers() {
  FakeChip chip;
  chip.busyReads = 1000000;
  Atecc608 se(chip);
  uint8_t pub[64];
  TEST_ASSERT_EQUAL(Result::Timeout, se.publicKey(0, pub));
  TEST_ASSERT_EQUAL_UINT(3, chip.packets.size());
  TEST_ASSERT_TRUE(chip.delayedUs >= 3 * 2500000ull);
  TEST_ASSERT_TRUE(chip.delayedUs < 3 * 2700000ull);
}

static void test_der() {
  uint8_t rs[64];
  uint8_t out[80];
  // High bit set in r: a 0x00 pad keeps the INTEGER positive.
  rs[0] = 0x80;
  rs[32] = 0x7F;
  for (int i = 1; i < 32; ++i) rs[i] = rs[32 + i] = static_cast<uint8_t>(i);
  size_t n = ecdsaDer(rs, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT(71, n);
  const Buf head{0x30, 0x45, 0x02, 0x21, 0x00, 0x80};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(head.data(), out, head.size());
  const Buf mid{0x02, 0x20, 0x7F, 0x01};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(mid.data(), out + 37, mid.size());
  TEST_ASSERT_EQUAL_UINT(0, ecdsaDer(rs, out, 70));

  // Leading zeros dropped; zero is one byte.
  memset(rs, 0, sizeof(rs));
  rs[31] = 0x05;
  n = ecdsaDer(rs, out, sizeof(out));
  assertBytes(Buf{0x30, 0x06, 0x02, 0x01, 0x05, 0x02, 0x01, 0x00}, out, n);

  // Both padded: the 72-byte maximum.
  memset(rs, 0xFF, sizeof(rs));
  TEST_ASSERT_EQUAL_UINT(72, ecdsaDer(rs, out, 72));
  TEST_ASSERT_EQUAL_HEX8(0x46, out[1]);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_crc);
  RUN_TEST(test_sign_flow_and_golden_packets);
  RUN_TEST(test_public_key);
  RUN_TEST(test_busy_chip_is_polled);
  RUN_TEST(test_wake_retried);
  RUN_TEST(test_no_chip_at_address);
  RUN_TEST(test_chip_left_awake_recovers);
  RUN_TEST(test_error_status_fails_at_once);
  RUN_TEST(test_transient_statuses_retried);
  RUN_TEST(test_bad_crc_retried);
  RUN_TEST(test_chip_that_never_answers);
  RUN_TEST(test_der);
  return UNITY_END();
}
