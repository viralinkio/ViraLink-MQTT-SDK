// Wire encoders for every ThingsBoard payload the library produces. All
// payload shapes live here so they can be checked against golden strings.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vl/Credentials.h"
#include "vl/core/JsonWriter.h"
#include "vl/core/Outbox.h"

namespace vl {

// Health keys, as bits of HealthSample::keys and Health::setKeys().
const uint16_t kHealthUptime = 1u << 0;
const uint16_t kHealthHeapFree = 1u << 1;
const uint16_t kHealthHeapMin = 1u << 2;
const uint16_t kHealthResetReason = 1u << 3;
const uint16_t kHealthRssi = 1u << 4;
const uint16_t kHealthIface = 1u << 5;
const uint16_t kHealthQueueDepth = 1u << 6;
const uint16_t kHealthDropped = 1u << 7;
const uint16_t kHealthDeadLetter = 1u << 8;
const uint16_t kHealthSdk = 1u << 9;
const uint16_t kHealthAll = (1u << 10) - 1;

// One health report. Only keys in `keys` are written, and a string key only
// when it is not null.
struct HealthSample {
  uint16_t keys = 0;
  uint32_t uptimeS = 0;
  uint32_t heapFree = 0;
  uint32_t heapMin = 0;
  const char* resetReason = nullptr;
  int16_t rssi = 0;
  const char* iface = nullptr;
  uint32_t queueDepth = 0;
  uint32_t dropped = 0;
  uint32_t deadLetters = 0;
  const char* sdk = nullptr;
};

namespace codec {

// Final publish payload for an outbox record. The stored payload is the bare
// values object; this adds the timestamp envelope and the gateway device
// wrapper. Returns the length, or 0 if it did not fit.
//   Telemetry     P                     or {"ts":T,"values":P}
//   GwTelemetry   {"Dev":[P]}           or {"Dev":[{"ts":T,"values":P}]}
//   GwAttributes  {"Dev":P}
//   GwClaim       {"Dev":P}
//   anything else P unchanged
size_t encodeRecord(const RecordView& rec, bool hasTs, uint64_t ts, const char* deviceName, char* out,
                    size_t cap);

// {"clientKeys":"a,b","sharedKeys":"c"}; a null list is omitted.
void attributeRequest(JsonWriter& w, const char* clientKeys, const char* sharedKeys);

// {"secretKey":…,"durationMs":…}
void claim(JsonWriter& w, const char* secretKey, uint32_t durationMs);

// {"error":…}: reply to a server RPC that failed.
void rpcError(JsonWriter& w, const char* message);

// {"method":…,"params":P}; params is pre-encoded JSON, {} when null.
void rpcRequest(JsonWriter& w, const char* method, const char* paramsJson, size_t paramsLen);

// {"current_fw_title":…,"current_fw_version":…}
void fwInfo(JsonWriter& w, const char* title, const char* version);
// {"fw_state":…}, plus "fw_error" when error is not null.
void fwState(JsonWriter& w, const char* state, const char* error);

// {"deviceName":…,"provisionDeviceKey":…,"provisionDeviceSecret":…} plus the
// credentials the device chose: "credentialsType" with "token" (AccessToken),
// "username"/"password"/"clientId" (MqttBasic) or "hash" = certPem (X509).
// A null deviceName or credential field is omitted; Kind::None lets the
// server generate an access token.
void provisionRequest(JsonWriter& w, const char* deviceName, const char* key, const char* secret,
                      const Credentials& creds, const char* certPem);

// {"vl_uptime":…,"vl_heap_free":…,…,"vl_sdk":…} in kHealth* bit order.
// v1Keys uses the 1.0.2 names "upTime", "ESP Free Heap" and
// "Viralink SDK Version"; keys 1.0.2 lacked keep their vl_ names.
void health(JsonWriter& w, const HealthSample& s, bool v1Keys);

// Gateway API
void gwConnect(JsonWriter& w, const char* device, const char* type);  // type may be null
void gwDisconnect(JsonWriter& w, const char* device);
// {"id":…,"device":…,"client":true,"keys":["a","b"]}; keys is comma-separated.
void gwAttributeRequest(JsonWriter& w, uint32_t id, const char* device, bool client, const char* keys);
// {"device":…,"id":…,"data":<json>}
void gwRpcReply(JsonWriter& w, const char* device, uint32_t id, const char* dataJson, size_t dataLen);

}  // namespace codec
}  // namespace vl
