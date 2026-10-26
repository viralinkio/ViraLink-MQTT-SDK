#include "vl/core/CertTime.h"

namespace vl {

// Days from 1970-01-01 (Howard Hinnant's days_from_civil). 32-bit arithmetic
// throughout: int is 16-bit on AVR.
static int32_t daysFromCivil(int32_t y, int32_t m, int32_t d) {
  y -= m <= 2;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const int32_t yoe = y - era * 400;
  const int32_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

int64_t epochSeconds(const UtcDate& d) {
  int64_t days = daysFromCivil(d.year, d.mon, d.day);
  return days * 86400 + static_cast<int64_t>(d.hour) * 3600 + static_cast<int64_t>(d.min) * 60 + d.sec;
}

uint8_t certValidity(const UtcDate& from, const UtcDate& to, bool haveNow, int64_t nowS, int64_t floorS) {
  uint8_t v = kCertValid;
  if (haveNow) {
    if (nowS > epochSeconds(to)) v |= kCertExpired;
    if (nowS < epochSeconds(from)) v |= kCertNotYetValid;
  } else if (epochSeconds(to) < floorS) {
    v |= kCertExpired;
  }
  return v;
}

}  // namespace vl
