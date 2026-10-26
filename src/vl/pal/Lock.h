#pragma once

#include "vl/Config.h"

#if VL_PLATFORM_ESP32
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

namespace vl {

class ILock {
 public:
  virtual ~ILock() {}
  virtual void lock() = 0;
  virtual void unlock() = 0;
};

class NullLock : public ILock {
 public:
  void lock() override {}
  void unlock() override {}
};

#if VL_PLATFORM_ESP32
// FreeRTOS mutex (priority inheritance), statically allocated. Never held
// across network I/O.
class FreeRtosLock : public ILock {
 public:
  FreeRtosLock() : handle_(xSemaphoreCreateMutexStatic(&storage_)) {}
  void lock() override { xSemaphoreTake(handle_, portMAX_DELAY); }
  void unlock() override { xSemaphoreGive(handle_); }

 private:
  StaticSemaphore_t storage_;
  SemaphoreHandle_t handle_;
};
typedef FreeRtosLock PlatformLock;
#else
typedef NullLock PlatformLock;
#endif

class LockGuard {
 public:
  explicit LockGuard(ILock& l) : l_(l) { l_.lock(); }
  ~LockGuard() { l_.unlock(); }
  LockGuard(const LockGuard&) = delete;
  LockGuard& operator=(const LockGuard&) = delete;

 private:
  ILock& l_;
};

}  // namespace vl
