#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vl/Config.h"
#include "vl/Span.h"

namespace vl {

// Read-only view of one JSON value inside a caller-owned buffer. parse()
// validates the whole document once; lookups then walk the bytes in place, so
// nothing is copied or allocated. A view is valid only as long as the buffer.
// Accessors on an Invalid value return Invalid or the default, so lookups
// chain safely: v["params"]["on"].asBool().
class JsonValue {
 public:
  enum class Type : uint8_t { Invalid, Null, Bool, Number, String, Array, Object };

  JsonValue() {}
  // Invalid unless [json, json + n) holds exactly one value (whitespace aside)
  // nested at most VL_JSON_MAX_DEPTH deep.
  static JsonValue parse(const char* json, size_t n);
  static JsonValue parse(Bytes b) { return parse(reinterpret_cast<const char*>(b.data()), b.size()); }

  Type type() const { return type_; }
  bool valid() const { return type_ != Type::Invalid; }

  // Object member (first match); Invalid if missing or not an object.
  JsonValue operator[](const char* key) const;
  // Array element; Invalid if out of range or not an array.
  JsonValue at(size_t i) const;
  // Members or elements; 0 for anything else.
  size_t size() const;
  // i-th object member; false past the end or if not an object.
  bool member(size_t i, JsonValue& key, JsonValue& value) const;

  bool asBool(bool def = false) const;
  // Numbers only; a fraction is truncated toward zero, out of range gives def.
  long long asInt(long long def = 0) const;
  double asDouble(double def = 0) const;
  // Unescaped UTF-8, NUL-terminated. False, with out untouched, if this is not
  // a string or it does not fit.
  bool copyString(char* out, size_t cap) const;
  // True if this is a string equal to s after unescaping.
  bool equals(const char* s) const;

  // The encoded value; strings include their quotes.
  const char* raw() const { return p_; }
  size_t rawSize() const { return n_; }

 private:
  JsonValue(const char* p, size_t n, Type t) : p_(p), n_(n), type_(t) {}
  // Steps over the member or element at p; false at the closing bracket.
  static bool next(const char*& p, const char* end, bool obj, JsonValue& key, JsonValue& value);
  const char* first() const;
  size_t decodedLength() const;

  const char* p_ = nullptr;
  size_t n_ = 0;
  Type type_ = Type::Invalid;
};

}  // namespace vl
