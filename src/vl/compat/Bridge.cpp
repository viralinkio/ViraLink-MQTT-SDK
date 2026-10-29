#include "vl/compat/Bridge.h"

#include <stdio.h>
#include <string.h>

#include "vl/Client.h"
#include "vl/Payload.h"
#include "vl/api/Gateway.h"
#include "vl/core/JsonReader.h"

namespace vl {
namespace compat {

namespace {

const char kDevicePrefix[] = "v1/devices/me/";
const char kResponsePrefix[] = "v1/devices/me/attributes/response/";

// The decimal id after prefix in topic; false if topic does not start with
// prefix or the rest is not a 32-bit number.
bool idAfter(const char* topic, const char* prefix, uint32_t& id) {
  size_t n = strlen(prefix);
  if (strncmp(topic, prefix, n) != 0 || !topic[n]) return false;
  uint64_t v = 0;
  for (const char* p = topic + n; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
    v = v * 10 + static_cast<uint64_t>(*p - '0');
    if (v > 0xFFFFFFFFull) return false;
  }
  id = static_cast<uint32_t>(v);
  return true;
}

bool isObject(const char* json, size_t len) {
  return JsonValue::parse(json, len).type() == JsonValue::Type::Object;
}

bool isArray(const char* json, size_t len) { return JsonValue::parse(json, len).type() == JsonValue::Type::Array; }

}  // namespace

Result Bridge::publish(const char* topic, const char* payload, size_t len) {
  if (!client_) return Result::NotReady;
  if (!topic || !payload) return Result::Rejected;
  uint32_t id = 0;
  if (strncmp(topic, kDevicePrefix, sizeof(kDevicePrefix) - 1) == 0) {
    const char* rest = topic + sizeof(kDevicePrefix) - 1;
    if (strcmp(rest, "telemetry") == 0) return telemetry(nullptr, payload, len);
    if (strcmp(rest, "attributes") == 0) return attributes(nullptr, payload, len);
    if (strcmp(rest, "claim") == 0) {
      if (!isObject(payload, len)) return Result::Rejected;
      RecordMeta m;
      m.topic = OutTopic::Claim;
      m.cls = RecordClass::Control;
      m.transient = true;
      return client_->enqueue(m, reinterpret_cast<const uint8_t*>(payload), len);
    }
    if (idAfter(topic, "v1/devices/me/attributes/request/", id)) {
      return raw(OutTopic::AttrRequest, RecordClass::Control, id, payload, len);
    }
    if (idAfter(topic, "v1/devices/me/rpc/response/", id)) {
      return raw(OutTopic::RpcResponse, RecordClass::Priority, id, payload, len);
    }
    if (idAfter(topic, "v1/devices/me/rpc/request/", id)) {
      return raw(OutTopic::RpcRequest, RecordClass::Data, id, payload, len);
    }
    return Result::Rejected;
  }
  if (strcmp(topic, "v1/gateway/connect") == 0) return raw(OutTopic::GwConnect, RecordClass::Control, 0, payload, len);
  if (strcmp(topic, "v1/gateway/disconnect") == 0) {
    return raw(OutTopic::GwDisconnect, RecordClass::Control, 0, payload, len);
  }
  if (strcmp(topic, "v1/gateway/attributes/request") == 0) {
    return raw(OutTopic::GwAttrRequest, RecordClass::Control, 0, payload, len);
  }
  if (strcmp(topic, "v1/gateway/rpc") == 0) return raw(OutTopic::GwRpc, RecordClass::Priority, 0, payload, len);
  return Result::Rejected;
}

Result Bridge::raw(OutTopic topic, RecordClass cls, uint32_t arg, const char* payload, size_t len) {
  if (!JsonValue::parse(payload, len).valid()) return Result::Rejected;
  RecordMeta m;
  m.topic = topic;
  m.cls = cls;
  m.arg = arg;
  return client_->enqueue(m, reinterpret_cast<const uint8_t*>(payload), len);
}

Result Bridge::values(OutTopic topic, uint16_t dev, bool stamp, const char* json, size_t len) {
  JsonValue v = JsonValue::parse(json, len);
  if (v.type() == JsonValue::Type::Array && stamp) {
    if (v.size() == 0) return Result::Rejected;
    for (size_t i = 0; i < v.size(); ++i) {
      JsonValue e = v.at(i);
      if (e.type() != JsonValue::Type::Object) return Result::Rejected;
      Result r = values(topic, dev, stamp, e.raw(), e.rawSize());
      if (!ok(r)) return r;
    }
    return Result::Queued;
  }
  if (v.type() != JsonValue::Type::Object) return Result::Rejected;
  JsonValue ts = v["ts"];
  JsonValue inner = v["values"];
  if (stamp && v.size() == 2 && ts.type() == JsonValue::Type::Number && inner.type() == JsonValue::Type::Object) {
    long long t = ts.asInt(-1);
    if (t < 0) return Result::Rejected;
    return enqueueValues(*client_, topic, dev, false, true, static_cast<uint64_t>(t), inner.raw(), inner.rawSize());
  }
  return enqueueValues(*client_, topic, dev, stamp, false, 0, v.raw(), v.rawSize());
}

Result Bridge::subDevice(const char* device, uint16_t& index) {
#if VL_ENABLE_GATEWAY
  if (!gw_) return Result::NotReady;
  SubDevice d = gw_->device(device);
  if (!d.valid()) return d.status();
  LockGuard g(client_->lock());
  index = client_->deviceIndex(device);
  return index == kNoDevice ? Result::NoMemory : Result::Ok;
#else
  (void)device;
  (void)index;
  return Result::NotReady;
#endif
}

Result Bridge::telemetry(const char* device, const char* json, size_t len) {
  if (!client_) return Result::NotReady;
  if (!json) return Result::Rejected;
  if (!device || !device[0]) return values(OutTopic::Telemetry, kNoDevice, true, json, len);
  // Checked before the sub-device is connected for it.
  if (!isObject(json, len) && !isArray(json, len)) return Result::Rejected;
  uint16_t index = kNoDevice;
  Result r = subDevice(device, index);
  return r == Result::Ok ? values(OutTopic::GwTelemetry, index, true, json, len) : r;
}

Result Bridge::attributes(const char* device, const char* json, size_t len) {
  if (!client_) return Result::NotReady;
  if (!json) return Result::Rejected;
  if (!device || !device[0]) return values(OutTopic::Attributes, kNoDevice, false, json, len);
  if (!isObject(json, len)) return Result::Rejected;
  uint16_t index = kNoDevice;
  Result r = subDevice(device, index);
  return r == Result::Ok ? values(OutTopic::GwAttributes, index, false, json, len) : r;
}

Result Bridge::claim(const char* device, const char* secretKey, uint32_t durationMs) {
  if (!client_) return Result::NotReady;
  if (!device || !device[0]) return client_->claim(secretKey, durationMs);
#if VL_ENABLE_GATEWAY
  if (!gw_) return Result::NotReady;
  return gw_->device(device).claim(secretKey, durationMs);
#else
  return Result::NotReady;
#endif
}

Result Bridge::connectDevice(const char* device) {
  if (!client_) return Result::NotReady;
#if VL_ENABLE_GATEWAY
  if (!gw_) return Result::NotReady;
  return gw_->device(device).status();
#else
  (void)device;
  return Result::NotReady;
#endif
}

Result Bridge::disconnectDevice(const char* device) {
  if (!client_) return Result::NotReady;
#if VL_ENABLE_GATEWAY
  if (!gw_) return Result::NotReady;
  return gw_->disconnect(device);
#else
  (void)device;
  return Result::NotReady;
#endif
}

Result Bridge::requestAttributes(const char* keysJson, size_t len, MessageCb cb, void* ctx, uint32_t timeoutMs) {
  if (!client_) return Result::NotReady;
  if (!keysJson || !isObject(keysJson, len)) return Result::Rejected;
  Request* q = nullptr;
  {
    LockGuard g(client_->lock());
    for (size_t i = 0; i < VL_COMPAT_MAX_REQUESTS && !q; ++i) {
      if (!requests_[i].used) q = &requests_[i];
    }
    if (q) {
      Request fresh = {this, 0, cb, ctx, true};
      *q = fresh;
    }
  }
  if (!q) return Result::NoMemory;
  uint32_t id = client_->addPending(timeoutMs, onResponse, q, this);
  if (id == 0) {
    LockGuard g(client_->lock());
    *q = Request();
    return Result::NoMemory;
  }
  {
    LockGuard g(client_->lock());
    q->id = id;  // the response topic carries it
  }
  RecordMeta m;
  m.topic = OutTopic::AttrRequest;
  m.cls = RecordClass::Control;
  m.arg = id;
  Result r = client_->enqueue(m, reinterpret_cast<const uint8_t*>(keysJson), len);
  // If the cancel loses a race with the timeout, onResponse frees the slot.
  if (!ok(r) && client_->cancelPending(id)) {
    LockGuard g(client_->lock());
    *q = Request();
  }
  return r;
}

void Bridge::onResponse(Result r, const uint8_t* p, size_t n, void* ctx) {
  Request* q = static_cast<Request*>(ctx);
  Request done;
  {
    LockGuard g(q->self->client_->lock());
    done = *q;
    *q = Request();
  }
  if (r != Result::Ok || !done.cb) return;
  char topic[sizeof(kResponsePrefix) + 10];
  snprintf(topic, sizeof(topic), "%s%lu", kResponsePrefix, static_cast<unsigned long>(done.id));
  done.cb(topic, p, n, done.ctx);
}

void Bridge::onDetach() {
  // The client dropped our pending requests without firing them.
  if (client_) {
    LockGuard g(client_->lock());
    for (size_t i = 0; i < VL_COMPAT_MAX_REQUESTS; ++i) requests_[i] = Request();
  }
  ApiModule::onDetach();
}

void Bridge::topics(TopicSink& sink) {
  sink.add(InTopic::RpcRequest);
  sink.add(InTopic::AttrUpdate);
  sink.add(InTopic::AttrResponse);
#if VL_ENABLE_GATEWAY
  if (gw_) {
    sink.add(InTopic::GwRpc);
    sink.add(InTopic::GwAttrUpdate);
  }
#endif
}

bool Bridge::forward(const char* topic, Bytes payload) {
  return cb_ && cb_(topic, payload.data(), payload.size(), ctx_);
}

bool Bridge::handle(const Topic& topic, Bytes payload) {
  if (!client_) return false;
  switch (topic.kind) {
    case InTopic::AttrResponse:
      if (client_->resolvePending(topic.id, payload.data(), payload.size())) return true;
      return forward(topic.raw, payload);
    case InTopic::RpcRequest:
    case InTopic::AttrUpdate:
    case InTopic::GwRpc:
    case InTopic::GwAttrUpdate:
      return forward(topic.raw, payload);
    default:
      return false;
  }
}

}  // namespace compat
}  // namespace vl
