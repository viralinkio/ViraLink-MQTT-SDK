#pragma once

#include <stddef.h>

#include "vl/Result.h"

namespace vl {

// Small durable blobs: credentials, and later OTA progress. Keys are at most
// 15 characters (the ESP32 NVS limit). Used on the client task, or before
// begin(); implementations need not be thread-safe.
class IKeyValueStore {
 public:
  virtual ~IKeyValueStore() {}
  // Copies the value of key into buf and its size into len. Rejected if the
  // key is missing, TooLarge if the value is longer than cap.
  virtual Result get(const char* key, void* buf, size_t cap, size_t& len) = 0;
  // Returns once the value is durable.
  virtual Result set(const char* key, const void* value, size_t len) = 0;
  // Ok also when the key was missing.
  virtual Result erase(const char* key) = 0;
};

}  // namespace vl
