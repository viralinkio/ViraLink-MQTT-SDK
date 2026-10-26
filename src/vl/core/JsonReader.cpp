#include "vl/core/JsonReader.h"

#include <stdlib.h>
#include <string.h>

namespace vl {

namespace {

const char* ws(const char* p, const char* end) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
  return p;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

uint32_t hex4(const char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v = (v << 4) | static_cast<uint32_t>(hexVal(p[i]));
  return v;
}

JsonValue::Type typeOf(char c) {
  switch (c) {
    case '"':
      return JsonValue::Type::String;
    case '{':
      return JsonValue::Type::Object;
    case '[':
      return JsonValue::Type::Array;
    case 't':
    case 'f':
      return JsonValue::Type::Bool;
    case 'n':
      return JsonValue::Type::Null;
    default:
      return JsonValue::Type::Number;
  }
}

// Each skip function takes p at the first byte of a token and returns the
// byte after it, or null if the token is malformed or runs past end.

const char* skipString(const char* p, const char* end) {
  for (++p; p < end;) {
    unsigned char c = static_cast<unsigned char>(*p);
    if (c == '"') return p + 1;
    if (c < 0x20) return nullptr;
    if (c != '\\') {
      ++p;
      continue;
    }
    if (++p >= end) return nullptr;
    if (*p == 'u') {
      if (end - p < 5) return nullptr;
      for (int i = 1; i <= 4; ++i) {
        if (hexVal(p[i]) < 0) return nullptr;
      }
      p += 5;
    } else if (*p && strchr("\"\\/bfnrt", *p)) {
      ++p;
    } else {
      return nullptr;
    }
  }
  return nullptr;
}

const char* skipDigits(const char* p, const char* end) {
  if (p >= end || !isDigit(*p)) return nullptr;
  while (p < end && isDigit(*p)) ++p;
  return p;
}

const char* skipNumber(const char* p, const char* end) {
  if (p < end && *p == '-') ++p;
  if (p < end && *p == '0') {
    ++p;
  } else if (!(p = skipDigits(p, end))) {
    return nullptr;
  }
  if (p < end && *p == '.' && !(p = skipDigits(p + 1, end))) return nullptr;
  if (p < end && (*p == 'e' || *p == 'E')) {
    ++p;
    if (p < end && (*p == '+' || *p == '-')) ++p;
    if (!(p = skipDigits(p, end))) return nullptr;
  }
  return p;
}

const char* skipLiteral(const char* p, const char* end, const char* lit) {
  size_t n = strlen(lit);
  if (static_cast<size_t>(end - p) < n || memcmp(p, lit, n) != 0) return nullptr;
  return p + n;
}

const char* skipValue(const char* p, const char* end, int depth);

const char* skipContainer(const char* p, const char* end, int depth) {
  if (depth <= 0) return nullptr;
  bool obj = *p == '{';
  char close = obj ? '}' : ']';
  p = ws(p + 1, end);
  if (p < end && *p == close) return p + 1;
  for (;;) {
    if (obj) {
      if (p >= end || *p != '"' || !(p = skipString(p, end))) return nullptr;
      p = ws(p, end);
      if (p >= end || *p != ':') return nullptr;
      p = ws(p + 1, end);
    }
    if (!(p = skipValue(p, end, depth - 1))) return nullptr;
    p = ws(p, end);
    if (p >= end) return nullptr;
    if (*p == close) return p + 1;
    if (*p != ',') return nullptr;
    p = ws(p + 1, end);
  }
}

// depth is the number of containers still allowed to open.
const char* skipValue(const char* p, const char* end, int depth) {
  if (p >= end) return nullptr;
  switch (*p) {
    case '"':
      return skipString(p, end);
    case '{':
    case '[':
      return skipContainer(p, end, depth);
    case 't':
      return skipLiteral(p, end, "true");
    case 'f':
      return skipLiteral(p, end, "false");
    case 'n':
      return skipLiteral(p, end, "null");
    default:
      return skipNumber(p, end);
  }
}

size_t encodeUtf8(uint32_t cp, char* out) {
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = static_cast<char>(0xF0 | (cp >> 18));
  out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (cp & 0x3F));
  return 4;
}

// Decodes one character of an already validated string body into UTF-8 and
// advances p. A surrogate pair is joined into one code point.
size_t decodeChar(const char*& p, char* out) {
  if (*p != '\\') {
    out[0] = *p++;
    return 1;
  }
  ++p;
  char e = *p++;
  switch (e) {
    case 'b':
      out[0] = '\b';
      return 1;
    case 'f':
      out[0] = '\f';
      return 1;
    case 'n':
      out[0] = '\n';
      return 1;
    case 'r':
      out[0] = '\r';
      return 1;
    case 't':
      out[0] = '\t';
      return 1;
    case 'u': {
      uint32_t cp = hex4(p);
      p += 4;
      // Validation guarantees four hex digits after any "\u".
      if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
        uint32_t lo = hex4(p + 2);
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          p += 6;
        }
      }
      return encodeUtf8(cp, out);
    }
    default:  // '"', '\\', '/'
      out[0] = e;
      return 1;
  }
}

}  // namespace

JsonValue JsonValue::parse(const char* json, size_t n) {
  if (!json) return JsonValue();
  const char* end = json + n;
  const char* p = ws(json, end);
  const char* e = skipValue(p, end, VL_JSON_MAX_DEPTH);
  if (!e || ws(e, end) != end) return JsonValue();
  return JsonValue(p, static_cast<size_t>(e - p), typeOf(*p));
}

const char* JsonValue::first() const { return ws(p_ + 1, p_ + n_); }

bool JsonValue::next(const char*& p, const char* end, bool obj, JsonValue& key, JsonValue& value) {
  if (p >= end || *p == '}' || *p == ']') return false;
  if (obj) {
    const char* k = skipString(p, end);
    key = JsonValue(p, static_cast<size_t>(k - p), Type::String);
    p = ws(ws(k, end) + 1, end);  // past ':'
  }
  const char* v = skipValue(p, end, VL_JSON_MAX_DEPTH);
  value = JsonValue(p, static_cast<size_t>(v - p), typeOf(*p));
  p = ws(v, end);
  if (p < end && *p == ',') p = ws(p + 1, end);
  return true;
}

JsonValue JsonValue::operator[](const char* key) const {
  if (type_ != Type::Object || !key) return JsonValue();
  const char* p = first();
  JsonValue k, v;
  while (next(p, p_ + n_, true, k, v)) {
    if (k.equals(key)) return v;
  }
  return JsonValue();
}

JsonValue JsonValue::at(size_t i) const {
  if (type_ != Type::Array) return JsonValue();
  const char* p = first();
  JsonValue k, v;
  for (size_t n = 0; next(p, p_ + n_, false, k, v); ++n) {
    if (n == i) return v;
  }
  return JsonValue();
}

size_t JsonValue::size() const {
  if (type_ != Type::Object && type_ != Type::Array) return 0;
  const char* p = first();
  JsonValue k, v;
  size_t n = 0;
  while (next(p, p_ + n_, type_ == Type::Object, k, v)) ++n;
  return n;
}

bool JsonValue::member(size_t i, JsonValue& key, JsonValue& value) const {
  if (type_ != Type::Object) return false;
  const char* p = first();
  for (size_t n = 0; next(p, p_ + n_, true, key, value); ++n) {
    if (n == i) return true;
  }
  return false;
}

bool JsonValue::asBool(bool def) const { return type_ == Type::Bool ? *p_ == 't' : def; }

long long JsonValue::asInt(long long def) const {
  if (type_ != Type::Number) return def;
  const char* p = p_;
  const char* end = p_ + n_;
  bool neg = *p == '-';
  if (neg) ++p;
  // Integers are parsed exactly; doubles cannot hold every 64-bit value.
  const unsigned long long limit = neg ? 9223372036854775808ull : 9223372036854775807ull;
  unsigned long long v = 0;
  for (; p < end && isDigit(*p); ++p) {
    unsigned d = static_cast<unsigned>(*p - '0');
    if (v > (limit - d) / 10) return def;
    v = v * 10 + d;
  }
  if (p == end) {
    if (!neg || v == 0) return static_cast<long long>(v);
    return -static_cast<long long>(v - 1) - 1;
  }
  double d = asDouble(0);
  if (!(d > -9223372036854775808.0 && d < 9223372036854775808.0)) return def;
  return static_cast<long long>(d);
}

double JsonValue::asDouble(double def) const {
  if (type_ != Type::Number) return def;
  char buf[40];
  if (n_ >= sizeof(buf)) return def;
  memcpy(buf, p_, n_);
  buf[n_] = '\0';
  return strtod(buf, nullptr);
}

size_t JsonValue::decodedLength() const {
  const char* p = p_ + 1;
  const char* end = p_ + n_ - 1;  // closing quote
  size_t len = 0;
  char ch[4];
  while (p < end) len += decodeChar(p, ch);
  return len;
}

bool JsonValue::copyString(char* out, size_t cap) const {
  if (type_ != Type::String || !out || decodedLength() >= cap) return false;
  const char* p = p_ + 1;
  const char* end = p_ + n_ - 1;
  size_t len = 0;
  while (p < end) len += decodeChar(p, out + len);
  out[len] = '\0';
  return true;
}

bool JsonValue::equals(const char* s) const {
  if (type_ != Type::String || !s) return false;
  const char* p = p_ + 1;
  const char* end = p_ + n_ - 1;
  char ch[4];
  while (p < end) {
    size_t k = decodeChar(p, ch);
    for (size_t i = 0; i < k; ++i, ++s) {
      if (*s == '\0' || *s != ch[i]) return false;
    }
  }
  return *s == '\0';
}

}  // namespace vl
