#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vl {

// Streaming JSON writer into a caller-owned buffer. No heap, no ArduinoJson.
// Commas are inserted automatically. On overflow every later call is a no-op
// and overflowed() returns true; the buffer always stays NUL-terminated.
class JsonWriter {
 public:
  JsonWriter(char* buf, size_t cap);

  JsonWriter& beginObject();
  JsonWriter& endObject();
  JsonWriter& beginArray();
  JsonWriter& endArray();
  JsonWriter& key(const char* k);

  JsonWriter& value(const char* s);
  JsonWriter& value(bool b);
  // One overload per fundamental type, so int32_t/int64_t resolve without
  // ambiguity whether the target typedefs them as int, long or long long.
  JsonWriter& value(int v) { return value(static_cast<long long>(v)); }
  JsonWriter& value(unsigned v) { return value(static_cast<unsigned long long>(v)); }
  JsonWriter& value(long v) { return value(static_cast<long long>(v)); }
  JsonWriter& value(unsigned long v) { return value(static_cast<unsigned long long>(v)); }
  JsonWriter& value(long long v);
  JsonWriter& value(unsigned long long v);
  JsonWriter& value(float v) { return value(static_cast<double>(v)); }
  JsonWriter& value(double v);
  JsonWriter& null();
  // Already-encoded JSON (an object, array or literal).
  JsonWriter& raw(const char* json, size_t n);

  // Points the writer at a copy of its buffer (used when a builder is copied).
  void rebind(char* buf) { buf_ = buf; }

  size_t size() const { return len_; }
  const char* c_str() const { return buf_; }
  bool overflowed() const { return overflow_; }
  // True once every opened object/array is closed.
  bool complete() const { return depth_ == 0 && len_ > 0 && !overflow_; }

 private:
  void separator();
  void put(char c);
  void put(const char* s, size_t n);
  void putEscaped(const char* s);

  char* buf_;
  size_t cap_;
  size_t len_ = 0;
  uint32_t needComma_ = 0;  // bit per nesting level
  uint8_t depth_ = 0;
  bool afterKey_ = false;
  bool overflow_ = false;
};

namespace json {

// Writes a finite v with up to `digits` (1-9) significant digits in printf
// "%.<digits>g" layout, without printf: AVR and newlib-nano builds have no
// float printf. out needs 24 bytes; returns the length and NUL-terminates.
size_t formatDouble(double v, int digits, char* out);

}  // namespace json

}  // namespace vl
