// UtcOffset, and setUtcMs() moving the host platform clock.
#include <unity.h>

#include "vl/Config.h"
#include "vl/pal/Clock.h"

using namespace vl;

void setUp() {}
void tearDown() {}

static void test_utc_offset() {
  UtcOffset o;
  uint64_t utc = 0;
  TEST_ASSERT_FALSE(o.get(5000, utc));
  o.set(1800000000000ull, 5000);
  TEST_ASSERT_TRUE(o.get(5000, utc));
  TEST_ASSERT_EQUAL_UINT64(1800000000000ull, utc);
  TEST_ASSERT_TRUE(o.get(65000, utc));
  TEST_ASSERT_EQUAL_UINT64(1800000060000ull, utc);
  o.set(1800000100000ull, 70000);  // resynced
  TEST_ASSERT_TRUE(o.get(70000, utc));
  TEST_ASSERT_EQUAL_UINT64(1800000100000ull, utc);
}

static void test_set_utc_moves_the_platform_clock() {
  const uint64_t floorMs = static_cast<uint64_t>(VL_TIME_FLOOR_S) * 1000u;
  TEST_ASSERT_FALSE(setUtcMs(floorMs - 1));  // before the floor: refused
  const uint64_t target = floorMs + 123456789ull;
  TEST_ASSERT_TRUE(setUtcMs(target));
  uint64_t utc = 0;
  TEST_ASSERT_TRUE(platformClock().utcMs(utc));
  TEST_ASSERT_TRUE(utc >= target);
  TEST_ASSERT_TRUE(utc < target + 5000);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_utc_offset);
  RUN_TEST(test_set_utc_moves_the_platform_clock);
  return UNITY_END();
}
