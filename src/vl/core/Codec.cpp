#include "vl/core/Codec.h"

#include <string.h>

namespace vl {
namespace codec {

namespace {

void valuesWithTs(JsonWriter& w, const RecordView& rec, bool hasTs, uint64_t ts) {
  const char* p = reinterpret_cast<const char*>(rec.payload);
  if (hasTs) {
    w.beginObject().key("ts").value(static_cast<unsigned long long>(ts)).key("values").raw(p, rec.len).endObject();
  } else {
    w.raw(p, rec.len);
  }
}

}  // namespace

size_t encodeRecord(const RecordView& rec, bool hasTs, uint64_t ts, const char* deviceName, char* out,
                    size_t cap) {
  JsonWriter w(out, cap);
  switch (rec.meta.topic) {
    case OutTopic::Telemetry:
      valuesWithTs(w, rec, hasTs, ts);
      break;
    case OutTopic::GwTelemetry:
      if (!deviceName) return 0;
      w.beginObject().key(deviceName).beginArray();
      valuesWithTs(w, rec, hasTs, ts);
      w.endArray().endObject();
      break;
    case OutTopic::GwAttributes:
    case OutTopic::GwClaim:
      if (!deviceName) return 0;
      w.beginObject().key(deviceName).raw(reinterpret_cast<const char*>(rec.payload), rec.len).endObject();
      break;
    default:
      w.raw(reinterpret_cast<const char*>(rec.payload), rec.len);
      break;
  }
  return w.overflowed() ? 0 : w.size();
}

void attributeRequest(JsonWriter& w, const char* clientKeys, const char* sharedKeys) {
  w.beginObject();
  if (clientKeys) w.key("clientKeys").value(clientKeys);
  if (sharedKeys) w.key("sharedKeys").value(sharedKeys);
  w.endObject();
}

void claim(JsonWriter& w, const char* secretKey, uint32_t durationMs) {
  w.beginObject();
  if (secretKey) w.key("secretKey").value(secretKey);
  w.key("durationMs").value(static_cast<unsigned long>(durationMs)).endObject();
}

void rpcError(JsonWriter& w, const char* message) {
  w.beginObject().key("error").value(message).endObject();
}

void rpcRequest(JsonWriter& w, const char* method, const char* paramsJson, size_t paramsLen) {
  w.beginObject().key("method").value(method).key("params");
  if (paramsJson && paramsLen) {
    w.raw(paramsJson, paramsLen);
  } else {
    w.beginObject().endObject();
  }
  w.endObject();
}

void fwInfo(JsonWriter& w, const char* title, const char* version) {
  w.beginObject().key("current_fw_title").value(title).key("current_fw_version").value(version).endObject();
}

void fwState(JsonWriter& w, const char* state, const char* error) {
  w.beginObject().key("fw_state").value(state);
  if (error) w.key("fw_error").value(error);
  w.endObject();
}

void provisionRequest(JsonWriter& w, const char* deviceName, const char* key, const char* secret,
                      const Credentials& creds, const char* certPem) {
  w.beginObject();
  if (deviceName) w.key("deviceName").value(deviceName);
  w.key("provisionDeviceKey").value(key).key("provisionDeviceSecret").value(secret);
  switch (creds.kind) {
    case Credentials::Kind::AccessToken:
      w.key("credentialsType").value("ACCESS_TOKEN").key("token").value(creds.username);
      break;
    case Credentials::Kind::MqttBasic:
      w.key("credentialsType").value("MQTT_BASIC");
      if (creds.username) w.key("username").value(creds.username);
      if (creds.password) w.key("password").value(creds.password);
      if (creds.clientId) w.key("clientId").value(creds.clientId);
      break;
    case Credentials::Kind::X509:
      w.key("credentialsType").value("X509_CERTIFICATE").key("hash").value(certPem);
      break;
    case Credentials::Kind::None:
      break;
  }
  w.endObject();
}

void gwConnect(JsonWriter& w, const char* device, const char* type) {
  w.beginObject().key("device").value(device);
  if (type) w.key("type").value(type);
  w.endObject();
}

void gwDisconnect(JsonWriter& w, const char* device) { w.beginObject().key("device").value(device).endObject(); }

void gwAttributeRequest(JsonWriter& w, uint32_t id, const char* device, bool client, const char* keys) {
  w.beginObject()
      .key("id")
      .value(static_cast<unsigned long>(id))
      .key("device")
      .value(device)
      .key("client")
      .value(client)
      .key("keys")
      .beginArray();
  char key[64];
  const char* s = keys ? keys : "";
  while (*s) {
    const char* comma = strchr(s, ',');
    size_t n = comma ? static_cast<size_t>(comma - s) : strlen(s);
    if (n >= sizeof(key)) n = sizeof(key) - 1;
    memcpy(key, s, n);
    key[n] = '\0';
    if (n) w.value(key);
    s += comma ? (comma - s) + 1 : static_cast<ptrdiff_t>(strlen(s));
  }
  w.endArray().endObject();
}

void gwRpcReply(JsonWriter& w, const char* device, uint32_t id, const char* dataJson, size_t dataLen) {
  w.beginObject()
      .key("device")
      .value(device)
      .key("id")
      .value(static_cast<unsigned long>(id))
      .key("data")
      .raw(dataJson, dataLen)
      .endObject();
}

}  // namespace codec
}  // namespace vl
