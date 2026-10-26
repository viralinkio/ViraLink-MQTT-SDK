#pragma once

#include "vl/Config.h"

#if VL_PLATFORM_ESP32

#include <stddef.h>
#include <stdint.h>

#include "vl/pal/KeyValueStore.h"

namespace vl {

// IKeyValueStore over ESP32 NVS, for Arduino and ESP-IDF. Values are blobs in
// one NVS namespace, committed on every write. Enable NVS encryption
// (CONFIG_NVS_ENCRYPTION) to keep credentials encrypted at rest. The NVS
// partition is initialized on first use if the application has not done it;
// a partition that needs erasing is reported, never erased here.
class NvsStore : public IKeyValueStore {
 public:
  // ns: NVS namespace, at most 15 characters; not copied (use a literal).
  explicit NvsStore(const char* ns = "vl") : ns_(ns) {}
  ~NvsStore() override;

  Result get(const char* key, void* buf, size_t cap, size_t& len) override;
  Result set(const char* key, const void* value, size_t len) override;
  Result erase(const char* key) override;

 private:
  bool open();

  const char* ns_;
  uint32_t handle_ = 0;  // nvs_handle_t
  bool open_ = false;
};

}  // namespace vl

#endif  // VL_PLATFORM_ESP32
