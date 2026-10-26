// Compile-time switches and default sizes. Every value can be overridden with
// a -D build flag (PlatformIO build_flags, Kconfig-free on ESP-IDF via
// target_compile_definitions, or a #define before including ViraLink.h).
#pragma once

#define VL_SDK_VERSION "2.0.0-alpha.1"

#if defined(ESP_PLATFORM) && !defined(ESP8266)
#define VL_PLATFORM_ESP32 1
#else
#define VL_PLATFORM_ESP32 0
#endif

#if defined(__AVR__)
#define VL_PROFILE_LITE 1
#else
#define VL_PROFILE_LITE 0
#endif

#ifndef VL_ENABLE_OTA
#define VL_ENABLE_OTA (!VL_PROFILE_LITE)
#endif
#ifndef VL_ENABLE_GATEWAY
#define VL_ENABLE_GATEWAY (!VL_PROFILE_LITE)
#endif
#ifndef VL_ENABLE_PROVISION
#define VL_ENABLE_PROVISION (!VL_PROFILE_LITE)
#endif
#ifndef VL_ENABLE_PERSISTENCE
#define VL_ENABLE_PERSISTENCE 0
#endif
#ifndef VL_ENABLE_HEALTH
#define VL_ENABLE_HEALTH 1
#endif

// 0 = off, 1 = error, 2 = warn, 3 = info, 4 = debug
#ifndef VL_LOG_LEVEL
#define VL_LOG_LEVEL 2
#endif

// Largest encoded publish (topic excluded). Also the transport buffer size.
#ifndef VL_TX_BUFFER_SIZE
#if VL_PROFILE_LITE
#define VL_TX_BUFFER_SIZE 256
#elif VL_PLATFORM_ESP32
#define VL_TX_BUFFER_SIZE 4096
#else
#define VL_TX_BUFFER_SIZE 1024
#endif
#endif

// Default capacity of a PayloadBuilder returned by telemetry()/attributes().
#ifndef VL_BUILDER_SIZE
#define VL_BUILDER_SIZE (VL_PROFILE_LITE ? 96 : 256)
#endif

#ifndef VL_MAX_MODULES
#define VL_MAX_MODULES 8
#endif
#ifndef VL_MAX_SUBSCRIPTIONS
#define VL_MAX_SUBSCRIPTIONS 12
#endif
#ifndef VL_MAX_TASKS
#define VL_MAX_TASKS 8
#endif
#ifndef VL_MAX_PENDING
#define VL_MAX_PENDING (VL_PROFILE_LITE ? 2 : 8)
#endif

// Deepest object/array nesting accepted in an incoming payload; bounds the
// reader's recursion.
#ifndef VL_JSON_MAX_DEPTH
#define VL_JSON_MAX_DEPTH (VL_PROFILE_LITE ? 8 : 16)
#endif

// Significant digits for floating-point values: enough to round-trip a float.
// Where double is 32-bit (AVR) more digits would only print noise.
#ifndef VL_JSON_DOUBLE_DIGITS
#if defined(__SIZEOF_DOUBLE__) && __SIZEOF_DOUBLE__ < 8
#define VL_JSON_DOUBLE_DIGITS 7
#else
#define VL_JSON_DOUBLE_DIGITS 9
#endif
#endif

// RPC: longest method name + 1, reply body size, client RPCs in flight.
#ifndef VL_RPC_METHOD_SIZE
#define VL_RPC_METHOD_SIZE 32
#endif
#ifndef VL_RPC_REPLY_SIZE
#define VL_RPC_REPLY_SIZE (VL_PROFILE_LITE ? 96 : 256)
#endif
#ifndef VL_RPC_MAX_CALLS
#define VL_RPC_MAX_CALLS (VL_PROFILE_LITE ? 1 : 4)
#endif
// Client RPC timeout; shorter than the server's 60 s default.
#ifndef VL_RPC_TIMEOUT_MS
#define VL_RPC_TIMEOUT_MS 10000
#endif

// Attributes: longest key list + 1 per handler or binding, requests in flight.
#ifndef VL_ATTR_KEYS_SIZE
#define VL_ATTR_KEYS_SIZE (VL_PROFILE_LITE ? 32 : 64)
#endif
#ifndef VL_ATTR_MAX_REQUESTS
#define VL_ATTR_MAX_REQUESTS (VL_PROFILE_LITE ? 1 : 4)
#endif
#ifndef VL_ATTR_TIMEOUT_MS
#define VL_ATTR_TIMEOUT_MS 10000
#endif

// Gateway
#ifndef VL_GW_NAME_SIZE
#define VL_GW_NAME_SIZE 32  // sub-device name, including the NUL
#endif
#ifndef VL_GW_TYPE_SIZE
#define VL_GW_TYPE_SIZE 24  // device type (profile name), including the NUL
#endif
#ifndef VL_GW_MAX_REQUESTS
#define VL_GW_MAX_REQUESTS 4  // outstanding sub-device attribute requests
#endif

// OTA
#ifndef VL_OTA_TITLE_SIZE
#define VL_OTA_TITLE_SIZE 64  // fw_title, including the NUL
#endif
#ifndef VL_OTA_VERSION_SIZE
#define VL_OTA_VERSION_SIZE 32  // fw_version, including the NUL
#endif
#ifndef VL_OTA_URL_SIZE
#define VL_OTA_URL_SIZE 256  // fw_url, including the NUL
#endif
// MQTT chunk size to request; 0 = the largest the transport accepts.
#ifndef VL_OTA_CHUNK_SIZE
#define VL_OTA_CHUNK_SIZE 0
#endif
#ifndef VL_OTA_CHUNK_TIMEOUT_MS
#define VL_OTA_CHUNK_TIMEOUT_MS 10000
#endif
// Attempts after a chunk timeout or a broken download before the update fails.
#ifndef VL_OTA_RETRIES
#define VL_OTA_RETRIES 5
#endif
#ifndef VL_OTA_RETRY_DELAY_MS
#define VL_OTA_RETRY_DELAY_MS 2000
#endif
// fw_url downloads: bytes read per loop(), and how long a read may stall.
#ifndef VL_OTA_HTTP_BLOCK
#define VL_OTA_HTTP_BLOCK 1024
#endif
#ifndef VL_OTA_HTTP_TIMEOUT_MS
#define VL_OTA_HTTP_TIMEOUT_MS 5000
#endif
// Longest wait for fw_state UPDATING to be delivered before the restart.
#ifndef VL_OTA_RESTART_DELAY_MS
#define VL_OTA_RESTART_DELAY_MS 5000
#endif
// A new image that has not been Online this long after boot is rolled back.
#ifndef VL_OTA_CONFIRM_TIMEOUT_MS
#define VL_OTA_CONFIRM_TIMEOUT_MS 300000
#endif
// 1 = install packages that have no checksum. Off: an unverified image is refused.
#ifndef VL_OTA_ALLOW_UNVERIFIED
#define VL_OTA_ALLOW_UNVERIFIED 0
#endif

// Wall-clock floor (2026-01-01T00:00:00Z). Anything earlier is "clock unknown".
#ifndef VL_TIME_FLOOR_S
#define VL_TIME_FLOOR_S 1767225600UL
#endif
