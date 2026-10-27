// FlashRegion windows and FlashOutboxStore on an emulated NOR chip, up to
// vl::Persistence replaying records after a restart and a power cut.
#include <string.h>
#include <unity.h>

#include <string>
#include <vector>

#include "ClientRig.h"
#include "vl/api/Persistence.h"
#include "vl/flash/FlashOutboxStore.h"

using namespace vl;
using vltest::ClientRig;
using vltest::FakeNor;
using vltest::Published;

void setUp() {}
void tearDown() {}

namespace {

struct Boot : ClientRig {
  Persistence persist;
  explicit Boot(IOutboxStore& store) : persist(store) { c.use(persist); }
};

std::string payloads(const std::vector<Published>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) s += (i ? " " : "") + v[i].payload;
  return s;
}

}  // namespace

static void test_region_maps_into_the_flash() {
  FakeNor nor(64 * 1024);
  FlashRegion r(nor, 8192, 16384);
  TEST_ASSERT_EQUAL_UINT32(16384, r.size());
  TEST_ASSERT_EQUAL_UINT32(4096, r.eraseSize());
  const uint8_t data[3] = {1, 2, 3};
  TEST_ASSERT_EQUAL(Result::Ok, r.program(100, data, 3));
  TEST_ASSERT_EQUAL_MEMORY(data, &nor.mem[8192 + 100], 3);
  uint8_t back[3] = {};
  TEST_ASSERT_EQUAL(Result::Ok, r.read(100, back, 3));
  TEST_ASSERT_EQUAL_MEMORY(data, back, 3);
  TEST_ASSERT_EQUAL(Result::Ok, r.erase(0));
  TEST_ASSERT_EQUAL_UINT8(0xFF, nor.mem[8192 + 100]);
  TEST_ASSERT_EQUAL(Result::Ok, r.program(16383, data, 1));  // the last byte
  TEST_ASSERT_EQUAL_UINT8(0xFF, nor.mem[8192 + 16384]);       // the next region is untouched

  TEST_ASSERT_EQUAL(Result::Rejected, r.program(16383, data, 2));
  TEST_ASSERT_EQUAL(Result::Rejected, r.read(16384, back, 1));
  TEST_ASSERT_EQUAL(Result::Rejected, r.read(0xFFFFFFFFu, back, 2));  // no wrap-around
  TEST_ASSERT_EQUAL(Result::Rejected, r.erase(16384));
  TEST_ASSERT_EQUAL(Result::Rejected, r.erase(100));  // not a block start
}

static void test_bad_regions_refuse_everything() {
  FakeNor nor(64 * 1024);
  uint8_t b = 0;
  FlashRegion misaligned(nor, 100, 8192);
  FlashRegion oddSize(nor, 0, 5000);
  FlashRegion pastEnd(nor, 60 * 1024, 8192);
  FlashRegion* bad[] = {&misaligned, &oddSize, &pastEnd};
  for (FlashRegion* r : bad) {
    TEST_ASSERT_EQUAL_UINT32(0, r->size());
    TEST_ASSERT_EQUAL(Result::Rejected, r->read(0, &b, 1));
    TEST_ASSERT_EQUAL(Result::Rejected, r->program(0, &b, 1));
    TEST_ASSERT_EQUAL(Result::Rejected, r->erase(0));
  }
  nor.ready = false;  // a chip not identified yet
  FlashRegion early(nor, 0, 8192);
  TEST_ASSERT_EQUAL_UINT32(0, early.size());
  nor.ready = true;
  TEST_ASSERT_EQUAL_UINT32(8192, early.size());
}

static void test_store_geometry() {
  FakeNor nor(64 * 1024);
  FlashOutboxStore s(nor, 8, 4096);
  TEST_ASSERT_EQUAL_UINT(8, s.segmentCount());
  TEST_ASSERT_EQUAL_UINT(4096, s.segmentSize());
  FlashOutboxStore many(nor, 32, 4096);
  TEST_ASSERT_EQUAL_UINT(16, many.segmentCount());  // what fits
  FlashOutboxStore odd(nor, 4, 3000);
  TEST_ASSERT_EQUAL_UINT(0, odd.segmentCount());
  nor.ready = false;
  TEST_ASSERT_EQUAL_UINT(0, s.segmentCount());
}

static void test_store_reads_writes_and_erases_segments() {
  FakeNor nor(64 * 1024);
  FlashOutboxStore s(nor, 4, 8192);  // two blocks per segment
  const uint8_t data[4] = {0xA1, 0xA2, 0xA3, 0xA4};
  TEST_ASSERT_EQUAL(Result::Ok, s.write(1, 8190, data, 2));
  TEST_ASSERT_EQUAL_MEMORY(data, &nor.mem[8192 + 8190], 2);
  TEST_ASSERT_EQUAL(Result::Rejected, s.write(1, 8191, data, 2));  // past the segment
  TEST_ASSERT_EQUAL(Result::Rejected, s.write(4, 0, data, 1));
  TEST_ASSERT_EQUAL(Result::Ok, s.write(2, 0, data, 4));

  uint8_t buf[8];
  size_t got = 99;
  TEST_ASSERT_EQUAL(Result::Ok, s.read(1, 8188, buf, sizeof(buf), got));
  TEST_ASSERT_EQUAL_UINT(4, got);  // clamped to the segment
  TEST_ASSERT_EQUAL_UINT8(0xFF, buf[0]);
  TEST_ASSERT_EQUAL_UINT8(0xA1, buf[2]);
  TEST_ASSERT_EQUAL(Result::Ok, s.read(1, 8192, buf, 1, got));
  TEST_ASSERT_EQUAL_UINT(0, got);
  TEST_ASSERT_EQUAL(Result::Rejected, s.read(1, 8193, buf, 1, got));
  TEST_ASSERT_EQUAL(Result::Rejected, s.read(4, 0, buf, 1, got));

  nor.mem[8192] = 0;  // first block of segment 1
  TEST_ASSERT_EQUAL(Result::Ok, s.erase(1));
  TEST_ASSERT_EQUAL_INT(2, nor.erases);
  TEST_ASSERT_EQUAL_UINT8(0xFF, nor.mem[8192]);
  TEST_ASSERT_EQUAL_UINT8(0xFF, nor.mem[8192 + 8190]);
  TEST_ASSERT_EQUAL_UINT8(0xA1, nor.mem[16384]);  // segment 2 kept
  TEST_ASSERT_EQUAL(Result::Rejected, s.erase(4));
  TEST_ASSERT_EQUAL(Result::Ok, s.sync(1));

  nor.dead = true;
  TEST_ASSERT_EQUAL(Result::Rejected, s.write(3, 0, data, 1));
  TEST_ASSERT_EQUAL(Result::Rejected, s.erase(3));
}

static void test_persistence_replays_from_flash() {
  FakeNor nor(64 * 1024);
  FlashRegion outbox(nor, 16384, 6 * 4096);  // the chip's first 16 KB hold something else
  FlashOutboxStore store(outbox, 6, 4096);
  {
    Boot b(store);
    TEST_ASSERT_EQUAL(Result::Ok, b.persist.status());
    TEST_ASSERT_EQUAL(Result::Ok, b.c.begin(b.cfg));  // never connects
    b.c.telemetry().add("n", 1).send();
    b.c.telemetry().add("n", 2).send();
    TEST_ASSERT_EQUAL(Result::Ok, b.persist.flush());
  }
  TEST_ASSERT_FALSE(nor.overwrote);
  for (size_t i = 0; i < 16384; ++i) TEST_ASSERT_EQUAL_UINT8(0xFF, nor.mem[i]);
  Boot b(store);
  TEST_ASSERT_EQUAL(Result::Ok, b.persist.status());
  b.connect();
  TEST_ASSERT_EQUAL_STRING("{\"n\":1} {\"n\":2}", payloads(b.flush()).c_str());
}

static void test_power_cut_keeps_an_ordered_prefix() {
  int partial = 0, whole = 0;
  for (long budget = 0; budget < 400; budget += 7) {
    FakeNor nor(32 * 1024);
    FlashOutboxStore store(nor, 6, 4096);
    {
      Boot b(store);
      TEST_ASSERT_EQUAL(Result::Ok, b.persist.status());
      b.c.begin(b.cfg);
      TEST_ASSERT_EQUAL(Result::Ok, b.persist.flush());  // the store is set up before the cut
      nor.budget = budget;
      for (int n = 1; n <= 3; ++n) {
        b.c.telemetry().add("n", n).send();
        b.persist.flush();
      }
    }
    nor.powerCycle();
    Boot b(store);
    TEST_ASSERT_EQUAL_MESSAGE(Result::Ok, b.persist.status(), std::to_string(budget).c_str());
    b.connect();
    std::string got = payloads(b.flush());
    const char* allowed[] = {"", "{\"n\":1}", "{\"n\":1} {\"n\":2}", "{\"n\":1} {\"n\":2} {\"n\":3}"};
    bool ok = false;
    for (const char* a : allowed) ok = ok || got == a;
    TEST_ASSERT_TRUE_MESSAGE(ok, (std::to_string(budget) + ": " + got).c_str());
    TEST_ASSERT_FALSE(nor.overwrote);
    if (got == allowed[3]) {
      ++whole;
    } else {
      ++partial;
    }
  }
  TEST_ASSERT_TRUE(partial > 0);  // the budgets really cut the saves
  TEST_ASSERT_TRUE(whole > 0);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_region_maps_into_the_flash);
  RUN_TEST(test_bad_regions_refuse_everything);
  RUN_TEST(test_store_geometry);
  RUN_TEST(test_store_reads_writes_and_erases_segments);
  RUN_TEST(test_persistence_replays_from_flash);
  RUN_TEST(test_power_cut_keeps_an_ordered_prefix);
  return UNITY_END();
}
