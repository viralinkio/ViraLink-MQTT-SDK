#pragma once

#include <stdarg.h>

#include "vl/Config.h"

namespace vl {

enum class LogLevel : unsigned char { Error = 1, Warn = 2, Info = 3, Debug = 4 };

// Receives fully formatted lines. Never passed credentials: the library masks
// tokens and passwords before logging.
typedef void (*LogSink)(LogLevel level, const char* line, void* ctx);

// Replaces the platform default (ESP_LOG on ESP-IDF, nothing elsewhere).
void setLogSink(LogSink sink, void* ctx);

void logf(LogLevel level, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

}  // namespace vl

#if VL_LOG_LEVEL >= 1
#define VL_LOGE(...) ::vl::logf(::vl::LogLevel::Error, __VA_ARGS__)
#else
#define VL_LOGE(...) \
  do {                \
    if (0) ::vl::logf(::vl::LogLevel::Error, __VA_ARGS__); \
  } while (0)
#endif
#if VL_LOG_LEVEL >= 2
#define VL_LOGW(...) ::vl::logf(::vl::LogLevel::Warn, __VA_ARGS__)
#else
#define VL_LOGW(...) \
  do {                \
    if (0) ::vl::logf(::vl::LogLevel::Error, __VA_ARGS__); \
  } while (0)
#endif
#if VL_LOG_LEVEL >= 3
#define VL_LOGI(...) ::vl::logf(::vl::LogLevel::Info, __VA_ARGS__)
#else
#define VL_LOGI(...) \
  do {                \
    if (0) ::vl::logf(::vl::LogLevel::Error, __VA_ARGS__); \
  } while (0)
#endif
#if VL_LOG_LEVEL >= 4
#define VL_LOGD(...) ::vl::logf(::vl::LogLevel::Debug, __VA_ARGS__)
#else
#define VL_LOGD(...) \
  do {                \
    if (0) ::vl::logf(::vl::LogLevel::Error, __VA_ARGS__); \
  } while (0)
#endif
