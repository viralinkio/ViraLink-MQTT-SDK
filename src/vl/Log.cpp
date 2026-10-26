#include "vl/Log.h"

#include <stdio.h>

#include "vl/Result.h"

#if VL_PLATFORM_ESP32
#include "esp_log.h"
#endif

namespace vl {

namespace {

#if VL_PLATFORM_ESP32
void espSink(LogLevel level, const char* line, void*) {
  switch (level) {
    case LogLevel::Error: ESP_LOGE("vl", "%s", line); break;
    case LogLevel::Warn: ESP_LOGW("vl", "%s", line); break;
    case LogLevel::Info: ESP_LOGI("vl", "%s", line); break;
    case LogLevel::Debug: ESP_LOGD("vl", "%s", line); break;
  }
}
LogSink g_sink = espSink;
#else
LogSink g_sink = nullptr;
#endif
void* g_ctx = nullptr;

}  // namespace

void setLogSink(LogSink sink, void* ctx) {
  g_sink = sink;
  g_ctx = ctx;
}

void logf(LogLevel level, const char* fmt, ...) {
  if (!g_sink) return;
  char line[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  g_sink(level, line, g_ctx);
}

const char* toString(Result r) {
  switch (r) {
    case Result::Ok: return "Ok";
    case Result::Queued: return "Queued";
    case Result::TooLarge: return "TooLarge";
    case Result::QueueFull: return "QueueFull";
    case Result::NotReady: return "NotReady";
    case Result::Timeout: return "Timeout";
    case Result::Rejected: return "Rejected";
    case Result::NoMemory: return "NoMemory";
  }
  return "?";
}

}  // namespace vl
