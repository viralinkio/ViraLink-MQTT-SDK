#include "vl/core/JsonWriter.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vl/Config.h"

namespace vl {

JsonWriter::JsonWriter(char* buf, size_t cap) : buf_(buf), cap_(cap) {
  if (cap_ == 0) {
    overflow_ = true;
  } else {
    buf_[0] = '\0';
  }
}

void JsonWriter::put(char c) { put(&c, 1); }

void JsonWriter::put(const char* s, size_t n) {
  if (overflow_) return;
  if (len_ + n + 1 > cap_) {
    overflow_ = true;
    return;
  }
  memcpy(buf_ + len_, s, n);
  len_ += n;
  buf_[len_] = '\0';
}

void JsonWriter::separator() {
  if (afterKey_) {
    afterKey_ = false;
    return;
  }
  if (depth_ == 0) return;
  uint32_t bit = 1u << (depth_ - 1);
  if (needComma_ & bit) put(',');
  needComma_ |= bit;
}

void JsonWriter::putEscaped(const char* s) {
  put('"');
  for (; s && *s; ++s) {
    unsigned char c = static_cast<unsigned char>(*s);
    switch (c) {
      case '"': put("\\\"", 2); break;
      case '\\': put("\\\\", 2); break;
      case '\n': put("\\n", 2); break;
      case '\r': put("\\r", 2); break;
      case '\t': put("\\t", 2); break;
      default:
        if (c < 0x20) {
          char esc[7];
          snprintf(esc, sizeof(esc), "\\u%04x", c);
          put(esc, 6);
        } else {
          put(static_cast<char>(c));
        }
    }
  }
  put('"');
}

JsonWriter& JsonWriter::beginObject() {
  separator();
  put('{');
  if (depth_ >= 31) overflow_ = true;
  if (!overflow_) {
    ++depth_;
    needComma_ &= ~(1u << (depth_ - 1));
  }
  return *this;
}

JsonWriter& JsonWriter::endObject() {
  put('}');
  if (depth_ > 0) --depth_;
  return *this;
}

JsonWriter& JsonWriter::beginArray() {
  separator();
  put('[');
  if (depth_ >= 31) overflow_ = true;
  if (!overflow_) {
    ++depth_;
    needComma_ &= ~(1u << (depth_ - 1));
  }
  return *this;
}

JsonWriter& JsonWriter::endArray() {
  put(']');
  if (depth_ > 0) --depth_;
  return *this;
}

JsonWriter& JsonWriter::key(const char* k) {
  separator();
  putEscaped(k);
  put(':');
  afterKey_ = true;
  return *this;
}

JsonWriter& JsonWriter::value(const char* s) {
  if (!s) return null();
  separator();
  putEscaped(s);
  return *this;
}

JsonWriter& JsonWriter::value(bool b) {
  separator();
  if (b) {
    put("true", 4);
  } else {
    put("false", 5);
  }
  return *this;
}

JsonWriter& JsonWriter::value(long long v) {
  separator();
  char tmp[24];
  // Printed by hand: AVR printf has no %lld.
  bool neg = v < 0;
  unsigned long long u = neg ? static_cast<unsigned long long>(-(v + 1)) + 1u : static_cast<unsigned long long>(v);
  size_t i = sizeof(tmp);
  do {
    tmp[--i] = static_cast<char>('0' + (u % 10u));
    u /= 10u;
  } while (u);
  if (neg) tmp[--i] = '-';
  put(tmp + i, sizeof(tmp) - i);
  return *this;
}

JsonWriter& JsonWriter::value(unsigned long long v) {
  separator();
  char tmp[24];
  size_t i = sizeof(tmp);
  do {
    tmp[--i] = static_cast<char>('0' + (v % 10u));
    v /= 10u;
  } while (v);
  put(tmp + i, sizeof(tmp) - i);
  return *this;
}

JsonWriter& JsonWriter::value(double v) {
  if (isnan(v) || isinf(v)) return null();
  separator();
  char tmp[24];
  put(tmp, json::formatDouble(v, VL_JSON_DOUBLE_DIGITS, tmp));
  return *this;
}

JsonWriter& JsonWriter::null() {
  separator();
  put("null", 4);
  return *this;
}

JsonWriter& JsonWriter::raw(const char* json, size_t n) {
  separator();
  put(json, n);
  return *this;
}

namespace json {

namespace {

// 10^n; exact for n <= 22 with a 64-bit double.
double pow10(int n) {
  double r = 1, b = 10;
  for (; n; n >>= 1) {
    if (n & 1) r *= b;
    if (n > 1) b *= b;
  }
  return r;
}

double scale(double v, int k) {  // v * 10^k without overflowing the factor
  for (; k > 22; k -= 22) v *= 1e22;
  for (; k < -22; k += 22) v /= 1e22;
  return k >= 0 ? v * pow10(k) : v / pow10(-k);
}

}  // namespace

size_t formatDouble(double v, int digits, char* out) {
  if (digits < 1) digits = 1;
  if (digits > 9) digits = 9;
  char* o = out;
  if (v < 0) {
    *o++ = '-';
    v = -v;
  }
  if (v == 0) {  // -0 prints as 0
    out[0] = '0';
    out[1] = '\0';
    return 1;
  }
  // Mantissa m with exactly `digits` digits and decimal exponent e such that
  // v ~= m * 10^(e - digits + 1). log10 may be off by one; the loop corrects it.
  uint32_t lo = 1;
  for (int i = 1; i < digits; ++i) lo *= 10;
  const double hi = static_cast<double>(lo) * 10;
  int e = static_cast<int>(floor(log10(v)));
  double s = 0;
  for (int tries = 0; tries < 4; ++tries) {
    s = scale(v, digits - 1 - e);
    if (s >= hi - 0.5) {
      ++e;
    } else if (s < lo - 0.5) {
      --e;
    } else {
      break;
    }
  }
  uint32_t m = static_cast<uint32_t>(s + 0.5);
  char d[9];
  for (int i = digits - 1; i >= 0; --i, m /= 10) d[i] = static_cast<char>('0' + m % 10);
  int n = digits;
  while (n > 1 && d[n - 1] == '0') --n;

  if (e < -4 || e >= digits) {  // d[.ddd]e+XX
    *o++ = d[0];
    if (n > 1) {
      *o++ = '.';
      memcpy(o, d + 1, static_cast<size_t>(n - 1));
      o += n - 1;
    }
    *o++ = 'e';
    *o++ = e < 0 ? '-' : '+';
    int a = e < 0 ? -e : e;
    if (a >= 100) *o++ = static_cast<char>('0' + a / 100);
    *o++ = static_cast<char>('0' + a / 10 % 10);
    *o++ = static_cast<char>('0' + a % 10);
  } else if (e >= 0) {  // ddd[.ddd]
    for (int i = 0; i <= e; ++i) *o++ = i < n ? d[i] : '0';
    if (n > e + 1) {
      *o++ = '.';
      memcpy(o, d + e + 1, static_cast<size_t>(n - e - 1));
      o += n - e - 1;
    }
  } else {  // 0.000ddd
    *o++ = '0';
    *o++ = '.';
    for (int i = -1; i > e; --i) *o++ = '0';
    memcpy(o, d, static_cast<size_t>(n));
    o += n;
  }
  *o = '\0';
  return static_cast<size_t>(o - out);
}

}  // namespace json

}  // namespace vl
