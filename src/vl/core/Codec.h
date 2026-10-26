// Wire encoders for every ThingsBoard payload the library produces. All
// payload shapes live here so they can be checked against golden strings.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vl/Credentials.h"
#include "vl/core/JsonWriter.h"
#include "vl/core/Outbox.h"

namespace vl {
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

// Gateway API
void gwConnect(JsonWriter& w, const char* device, const char* type);  // type may be null
void gwDisconnect(JsonWriter& w, const char* device);
// {"id":…,"device":…,"client":true,"keys":["a","b"]}; keys is comma-separated.
void gwAttributeRequest(JsonWriter& w, uint32_t id, const char* device, bool client, const char* keys);
// {"device":…,"id":…,"data":<json>}
void gwRpcReply(JsonWriter& w, const char* device, uint32_t id, const char* dataJson, size_t dataLen);

}  // namespace codec
}  // namespace vl
