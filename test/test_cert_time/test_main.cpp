// Certificate dates: calendar to epoch (golden values from Python's
// calendar.timegm) and the validity policy with and without a clock.
#include <unity.h>

#include "vl/core/CertTime.h"

using namespace vl;

void setUp() {}
void tearDown() {}

namespace {

UtcDate date(int y, int mo, int d, int h = 0, int mi = 0, int s = 0) {
  UtcDate u = {y, mo, d, h, mi, s};
  return u;
}

const int64_t kFloor = 1767225600;  // 2026-01-01

}  // namespace

static void test_epoch_seconds() {
  TEST_ASSERT_EQUAL_INT64(0, epochSeconds(date(1970, 1, 1)));
  TEST_ASSERT_EQUAL_INT64(951782400, epochSeconds(date(2000, 2, 29)));
  TEST_ASSERT_EQUAL_INT64(1767225600, epochSeconds(date(2026, 1, 1)));
  TEST_ASSERT_EQUAL_INT64(2524607999LL, epochSeconds(date(2049, 12, 31, 23, 59, 59)));
  TEST_ASSERT_EQUAL_INT64(4102444800LL, epochSeconds(date(2100, 1, 1)));  // past 2038 and 2^32
  TEST_ASSERT_EQUAL_INT64(-86400, epochSeconds(date(1969, 12, 31)));
}

static void test_validity_with_a_clock() {
  UtcDate from = date(2025, 6, 1), to = date(2027, 6, 1);
  int64_t now = epochSeconds(date(2026, 10, 26, 12));
  TEST_ASSERT_EQUAL_UINT8(kCertValid, certValidity(from, to, true, now, kFloor));
  TEST_ASSERT_EQUAL_UINT8(kCertExpired, certValidity(from, date(2026, 10, 26, 11, 59, 59), true, now, kFloor));
  TEST_ASSERT_EQUAL_UINT8(kCertValid, certValidity(from, date(2026, 10, 26, 12), true, now, kFloor));  // inclusive
  TEST_ASSERT_EQUAL_UINT8(kCertNotYetValid, certValidity(date(2026, 10, 26, 12, 0, 1), to, true, now, kFloor));
}

static void test_validity_without_a_clock() {
  // Only an expiry before the floor is certain.
  TEST_ASSERT_EQUAL_UINT8(kCertExpired, certValidity(date(2020, 1, 1), date(2025, 12, 31), false, 0, kFloor));
  TEST_ASSERT_EQUAL_UINT8(kCertValid, certValidity(date(2020, 1, 1), date(2026, 1, 1), false, 0, kFloor));
  // Issued after the firmware was built: cannot be judged, so accepted.
  TEST_ASSERT_EQUAL_UINT8(kCertValid, certValidity(date(2030, 1, 1), date(2040, 1, 1), false, 0, kFloor));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_epoch_seconds);
  RUN_TEST(test_validity_with_a_clock);
  RUN_TEST(test_validity_without_a_clock);
  return UNITY_END();
}
