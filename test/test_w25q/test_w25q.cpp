// W25qFlash against an SPI NOR chip emulator: identification and wake-up,
// reads, page-split programs with write enable, erases, busy timeouts, write
// protection, and FlashKvStore running on top.
#include <string.h>
#include <unity.h>

#include <string>
#include <vector>

#include "Fakes.h"
#include "vl/flash/FlashKvStore.h"
#include "vl/flash/W25qFlash.h"

using namespace vl;
using vltest::FakeClock;

void setUp() {}
void tearDown() {}

namespace {

// Behaves like a W25Q: commands run at chip-select high; a page program
// wraps inside its page; programs and erases need write enable and leave the
// chip busy for a number of status polls; asleep it answers only 0xAB.
struct SpiNor : ISpiDevice {
  std::vector<uint8_t> mem;
  uint8_t id[3] = {0xEF, 0x40, 0x14};
  bool asleep = false;
  bool present = true;
  bool writeProtected = false;
  int busyPolls = 2;  // status polls a program or erase stays busy for
  bool stuck = false; // busy forever
  bool selected = false;
  bool wel = false;
  int busy = 0;
  bool wrapped = false;         // a page program ran past its page
  bool commandWhileBusy = false;
  std::vector<uint8_t> cmd;
  size_t readPos = 0;
  std::vector<std::string> log;  // one entry per transaction: opcode in hex

  explicit SpiNor(size_t bytes) : mem(bytes, 0xFF) {}

  void select() override {
    TEST_ASSERT_FALSE(selected);
    selected = true;
    cmd.clear();
    readPos = 0;
  }
  void write(const uint8_t* p, size_t n) override {
    TEST_ASSERT_TRUE(selected);
    cmd.insert(cmd.end(), p, p + n);
  }
  void read(uint8_t* buf, size_t n) override {
    TEST_ASSERT_TRUE(selected);
    for (size_t i = 0; i < n; ++i) buf[i] = next();
  }
  void deselect() override {
    TEST_ASSERT_TRUE(selected);
    selected = false;
    if (cmd.empty()) return;
    char op[4];
    snprintf(op, sizeof(op), "%02X", cmd[0]);
    log.push_back(op);
    if (!present) return;
    if (asleep) {
      if (cmd[0] == 0xAB) asleep = false;
      return;
    }
    if (busy && cmd[0] != 0x05) commandWhileBusy = true;
    switch (cmd[0]) {
      case 0x06:
        wel = !writeProtected;
        break;
      case 0x02:
        if (wel && cmd.size() > 4) {
          uint32_t a = addr();
          for (size_t i = 4; i < cmd.size(); ++i) {
            uint32_t page = a & ~0xFFu;
            uint32_t at = page | ((a + static_cast<uint32_t>(i - 4)) & 0xFF);
            if (at != a + i - 4) wrapped = true;
            mem[at] &= cmd[i];
          }
          busy = busyPolls;
        }
        wel = false;
        break;
      case 0x20:
        if (wel) {
          memset(&mem[addr() & ~0xFFFu], 0xFF, 4096);
          busy = busyPolls;
        }
        wel = false;
        break;
    }
  }

 private:
  uint32_t addr() const { return (static_cast<uint32_t>(cmd[1]) << 16) | (cmd[2] << 8) | cmd[3]; }
  uint8_t next() {
    size_t pos = readPos++;
    if (!present || asleep || cmd.empty()) return 0xFF;
    switch (cmd[0]) {
      case 0x9F: return pos < 3 ? id[pos] : 0xFF;
      case 0x05: {
        uint8_t s = static_cast<uint8_t>((busy || stuck ? 1 : 0) | (wel ? 2 : 0));
        if (busy) --busy;
        return s;
      }
      case 0x03: return cmd.size() >= 4 ? mem[(addr() + pos) % mem.size()] : 0xFF;
      default: return 0xFF;
    }
  }
};

}  // namespace

static void test_begin_identifies_the_chip() {
  FakeClock clock;
  SpiNor chip(1 << 20);
  W25qFlash f(chip, &clock);
  TEST_ASSERT_EQUAL_UINT32(0, f.size());
  uint8_t b;
  TEST_ASSERT_EQUAL(Result::Rejected, f.read(0, &b, 1));
  TEST_ASSERT_EQUAL(Result::Ok, f.begin());
  TEST_ASSERT_EQUAL_UINT32(1u << 20, f.size());
  TEST_ASSERT_EQUAL_UINT32(4096, f.eraseSize());
  TEST_ASSERT_EQUAL_HEX32(0xEF4014, f.jedecId());
  TEST_ASSERT_EQUAL_STRING("AB", chip.log[0].c_str());  // woken first

  SpiNor big(1 << 20);
  big.id[2] = 0x19;  // 32 MB: only the first 16 MB with 3-byte addresses
  W25qFlash g(big, &clock);
  TEST_ASSERT_EQUAL(Result::Ok, g.begin());
  TEST_ASSERT_EQUAL_UINT32(16u << 20, g.size());
}

static void test_sleeping_chip_is_woken() {
  FakeClock clock;
  clock.step = 1;
  SpiNor chip(1 << 20);
  chip.asleep = true;
  W25qFlash f(chip, &clock);
  TEST_ASSERT_EQUAL(Result::Ok, f.begin());
  TEST_ASSERT_FALSE(chip.asleep);
  TEST_ASSERT_EQUAL_UINT32(1u << 20, f.size());
}

static void test_missing_or_odd_chip_refused() {
  FakeClock clock;
  clock.step = 1;
  SpiNor none(1 << 20);
  none.present = false;  // MISO floats high
  W25qFlash f(none, &clock);
  TEST_ASSERT_EQUAL(Result::Rejected, f.begin());
  TEST_ASSERT_EQUAL_UINT32(0, f.size());

  SpiNor zeros(1 << 20);
  zeros.id[0] = zeros.id[1] = zeros.id[2] = 0;  // MISO held low
  W25qFlash g(zeros, &clock);
  TEST_ASSERT_EQUAL(Result::Rejected, g.begin());

  SpiNor odd(1 << 20);
  odd.id[2] = 0x05;  // capacity code no SPI NOR uses
  W25qFlash h(odd, &clock);
  TEST_ASSERT_EQUAL(Result::Rejected, h.begin());
}

static void test_read() {
  FakeClock clock;
  SpiNor chip(1 << 20);
  for (size_t i = 0; i < 300; ++i) chip.mem[0x12300 + i] = static_cast<uint8_t>(i);
  W25qFlash f(chip, &clock);
  TEST_ASSERT_EQUAL(Result::Ok, f.begin());
  chip.log.clear();
  uint8_t buf[300];
  TEST_ASSERT_EQUAL(Result::Ok, f.read(0x12300, buf, sizeof(buf)));
  for (size_t i = 0; i < 300; ++i) TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(i), buf[i]);
  TEST_ASSERT_EQUAL_UINT(1, chip.log.size());
  TEST_ASSERT_EQUAL_STRING("03", chip.log[0].c_str());
  TEST_ASSERT_EQUAL(Result::Ok, f.read((1 << 20) - 1, buf, 1));
  TEST_ASSERT_EQUAL(Result::Rejected, f.read((1 << 20) - 1, buf, 2));
  TEST_ASSERT_EQUAL(Result::Rejected, f.read(0xFFFFFFFFu, buf, 2));
}

static void test_program_splits_pages() {
  FakeClock clock;
  SpiNor chip(1 << 20);
  W25qFlash f(chip, &clock);
  TEST_ASSERT_EQUAL(Result::Ok, f.begin());
  chip.log.clear();
  uint8_t data[600];
  for (size_t i = 0; i < sizeof(data); ++i) data[i] = static_cast<uint8_t>(i * 7);
  TEST_ASSERT_EQUAL(Result::Ok, f.program(250, data, sizeof(data)));  // 6 + 256 + 256 + 82
  TEST_ASSERT_FALSE(chip.wrapped);
  TEST_ASSERT_FALSE(chip.commandWhileBusy);
  TEST_ASSERT_EQUAL_MEMORY(data, &chip.mem[250], sizeof(data));
  int programs = 0, enables = 0;
  for (size_t i = 0; i < chip.log.size(); ++i) {
    if (chip.log[i] == "02") {
      ++programs;
      TEST_ASSERT_EQUAL_STRING("06", chip.log[i - 2].c_str());  // write enable before each page,
      TEST_ASSERT_EQUAL_STRING("05", chip.log[i - 1].c_str());  // confirmed in the status
    }
    if (chip.log[i] == "06") ++enables;
  }
  TEST_ASSERT_EQUAL_INT(4, programs);
  TEST_ASSERT_EQUAL_INT(4, enables);
  TEST_ASSERT_EQUAL_STRING("05", chip.log.back().c_str());  // waited for the last page

  TEST_ASSERT_EQUAL(Result::Ok, f.program(0, data, 0));
  TEST_ASSERT_EQUAL(Result::Rejected, f.program((1 << 20) - 1, data, 2));
}

static void test_erase() {
  FakeClock clock;
  SpiNor chip(1 << 20);
  memset(&chip.mem[0x3000], 0, 4096 + 1);
  W25qFlash f(chip, &clock);
  TEST_ASSERT_EQUAL(Result::Ok, f.begin());
  TEST_ASSERT_EQUAL(Result::Rejected, f.erase(0x3001));
  TEST_ASSERT_EQUAL(Result::Rejected, f.erase(1 << 20));
  TEST_ASSERT_EQUAL(Result::Ok, f.erase(0x3000));
  TEST_ASSERT_EQUAL_UINT8(0xFF, chip.mem[0x3000]);
  TEST_ASSERT_EQUAL_UINT8(0xFF, chip.mem[0x3FFF]);
  TEST_ASSERT_EQUAL_UINT8(0x00, chip.mem[0x4000]);
  TEST_ASSERT_FALSE(chip.commandWhileBusy);
}

static void test_busy_timeout_and_write_protect() {
  FakeClock clock;
  clock.step = 1;
  SpiNor chip(1 << 20);
  W25qFlash f(chip, &clock);
  TEST_ASSERT_EQUAL(Result::Ok, f.begin());
  chip.stuck = true;
  uint8_t b = 0;
  uint64_t t0 = clock.now;
  TEST_ASSERT_EQUAL(Result::Timeout, f.program(0, &b, 1));
  TEST_ASSERT_TRUE(clock.now - t0 >= 10);
  t0 = clock.now;
  TEST_ASSERT_EQUAL(Result::Timeout, f.erase(0));
  TEST_ASSERT_TRUE(clock.now - t0 >= 1000);
  TEST_ASSERT_TRUE(clock.now - t0 < 2000);

  SpiNor locked(1 << 20);
  locked.writeProtected = true;
  W25qFlash g(locked, &clock);
  TEST_ASSERT_EQUAL(Result::Ok, g.begin());
  TEST_ASSERT_EQUAL(Result::Rejected, g.program(0, &b, 1));
  TEST_ASSERT_EQUAL(Result::Rejected, g.erase(0));
  TEST_ASSERT_EQUAL_UINT8(0xFF, locked.mem[0]);
}

static void test_kv_store_on_the_chip() {
  FakeClock clock;
  SpiNor chip(1 << 20);
  W25qFlash f(chip, &clock);
  TEST_ASSERT_EQUAL(Result::Ok, f.begin());
  FlashRegion kv(f, 0xF0000, 2 * 4096);
  {
    FlashKvStore s(kv);
    TEST_ASSERT_EQUAL(Result::Ok, s.set("token", "abc123", 6));
  }
  FlashKvStore s(kv);
  char buf[16];
  size_t len = 0;
  TEST_ASSERT_EQUAL(Result::Ok, s.get("token", buf, sizeof(buf), len));
  TEST_ASSERT_EQUAL_UINT(6, len);
  TEST_ASSERT_EQUAL_MEMORY("abc123", buf, 6);
  for (size_t i = 0; i < 0xF0000; ++i) {
    if (chip.mem[i] != 0xFF) TEST_FAIL_MESSAGE("wrote outside the region");
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_begin_identifies_the_chip);
  RUN_TEST(test_sleeping_chip_is_woken);
  RUN_TEST(test_missing_or_odd_chip_refused);
  RUN_TEST(test_read);
  RUN_TEST(test_program_splits_pages);
  RUN_TEST(test_erase);
  RUN_TEST(test_busy_timeout_and_write_protect);
  RUN_TEST(test_kv_store_on_the_chip);
  return UNITY_END();
}
