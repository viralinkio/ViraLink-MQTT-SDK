// Certificate validity dates, judged by the library because the ESP32 mbedTLS
// build has no time source of its own (MBEDTLS_HAVE_TIME_DATE is off).
#pragma once

#include <stdint.h>

namespace vl {

// UTC calendar time, as X.509 dates carry it (fields match mbedtls_x509_time).
struct UtcDate {
  int year;
  int mon;  // 1-12
  int day;  // 1-31
  int hour;
  int min;
  int sec;
};

// Seconds since 1970-01-01T00:00:00Z (proleptic Gregorian calendar).
int64_t epochSeconds(const UtcDate& d);

enum CertValidity : uint8_t {
  kCertValid = 0,
  kCertExpired = 1,
  kCertNotYetValid = 2,
};

// With the wall clock known (haveNow), both dates are judged against nowS.
// Without it only an expiry before floorS is certain: a not-before date
// cannot be judged, and refusing it would refuse every certificate issued
// after the firmware was built.
uint8_t certValidity(const UtcDate& from, const UtcDate& to, bool haveNow, int64_t nowS, int64_t floorS);

}  // namespace vl
