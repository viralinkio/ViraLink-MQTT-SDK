// The slice of the Arduino core that ViraLinkCompat.h and its host test use.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>

inline void delay(unsigned long) {}
inline void delayMicroseconds(unsigned int) {}

class String {
 public:
  String(const char* s = "") : s_(s ? s : "") {}
  const char* c_str() const { return s_.c_str(); }
  unsigned int length() const { return static_cast<unsigned int>(s_.size()); }
  bool isEmpty() const { return s_.empty(); }
  bool operator==(const char* o) const { return s_ == o; }

 private:
  std::string s_;
};
