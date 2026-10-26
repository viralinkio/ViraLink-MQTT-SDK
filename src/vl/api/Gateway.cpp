#include "vl/api/Gateway.h"

#if VL_ENABLE_GATEWAY

#include <string.h>

#include "vl/Log.h"
#include "vl/core/Codec.h"

namespace vl {

const char* SubDevice::name() const { return valid() ? gw_->devices_[index_].name : ""; }

Result SubDevice::usable() const {
  if (status_ != Result::Ok) return status_;
  return gw_->client_ ? Result::Ok : Result::NotReady;
}

Result SubDevice::claim(const char* secretKey, uint32_t durationMs) const {
  Result r = usable();
  return r == Result::Ok ? gw_->claimAt(index_, secretKey, durationMs) : r;
}

Result SubDevice::disconnect() const {
  Result r = usable();
  return r == Result::Ok ? gw_->disconnectAt(index_) : r;
}

Result SubDevice::requestAttributes(bool client, const char* keys, GwAttrResponseCb cb, void* ctx,
                                    uint32_t timeoutMs) const {
  if (!keys || !keys[0] || !cb) return Result::Rejected;
  Result r = usable();
  return r == Result::Ok ? gw_->requestAt(index_, client, keys, cb, ctx, timeoutMs) : r;
}

size_t GatewayBase::lookup(const char* name) const {
  for (size_t i = 0; i < count_; ++i) {
    if (strcmp(devices_[i].name, name) == 0) return i;
  }
  return count_;
}

SubDevice GatewayBase::device(const char* name, const char* type) {
  size_t n = name ? strlen(name) : 0;
  size_t tn = type ? strlen(type) : 0;
  if (n == 0 || n >= VL_GW_NAME_SIZE || tn >= VL_GW_TYPE_SIZE) return SubDevice(this, 0, Result::Rejected);
  if (!client_) return SubDevice(this, 0, Result::NotReady);
  size_t index;
  bool connect = false;
  {
    LockGuard g(client_->lock());
    index = lookup(name);
    if (index == count_) {
      if (count_ >= cap_) return SubDevice(this, 0, Result::NoMemory);
      Device& d = devices_[count_++];
      memcpy(d.name, name, n + 1);
      memcpy(d.type, type ? type : "", tn + 1);
      d.connected = false;
    }
    if (!devices_[index].connected) {
      devices_[index].connected = true;
      connect = true;
    }
  }
  if (connect) {
    Result r = queueConnect(index, RecordClass::Control);
    if (!ok(r)) {
      LockGuard g(client_->lock());
      devices_[index].connected = false;  // the next device() call tries again
      return SubDevice(this, static_cast<uint16_t>(index), r);
    }
  }
  return SubDevice(this, static_cast<uint16_t>(index), Result::Ok);
}

SubDevice GatewayBase::find(const char* name) {
  if (!name || !client_) return SubDevice(this, 0, client_ ? Result::Rejected : Result::NotReady);
  LockGuard g(client_->lock());
  size_t index = lookup(name);
  if (index == count_) return SubDevice(this, 0, Result::Rejected);
  return SubDevice(this, static_cast<uint16_t>(index), Result::Ok);
}

Result GatewayBase::disconnect(const char* name) {
  SubDevice d = find(name);
  return d.valid() ? disconnectAt(d.index_) : d.status();
}

// Names and types never change once added, so they are read without the lock.
Result GatewayBase::queueConnect(size_t index, RecordClass cls) {
  const Device& d = devices_[index];
  char buf[2 * VL_GW_NAME_SIZE + 2 * VL_GW_TYPE_SIZE + 32];
  JsonWriter w(buf, sizeof(buf));
  codec::gwConnect(w, d.name, d.type[0] ? d.type : nullptr);
  if (w.overflowed()) return Result::TooLarge;
  RecordMeta m;
  m.topic = OutTopic::GwConnect;
  m.cls = cls;
  return client_->enqueue(m, reinterpret_cast<const uint8_t*>(buf), w.size());
}

Result GatewayBase::disconnectAt(size_t index) {
  {
    LockGuard g(client_->lock());
    if (!devices_[index].connected) return Result::Ok;
    devices_[index].connected = false;
  }
  char buf[2 * VL_GW_NAME_SIZE + 32];
  JsonWriter w(buf, sizeof(buf));
  codec::gwDisconnect(w, devices_[index].name);
  RecordMeta m;
  m.topic = OutTopic::GwDisconnect;
  m.cls = RecordClass::Control;
  Result r = w.overflowed() ? Result::TooLarge : client_->enqueue(m, reinterpret_cast<const uint8_t*>(buf), w.size());
  if (!ok(r)) {
    LockGuard g(client_->lock());
    devices_[index].connected = true;
  }
  return r;
}

Result GatewayBase::claimAt(size_t index, const char* secretKey, uint32_t durationMs) {
  char buf[128];
  JsonWriter w(buf, sizeof(buf));
  codec::claim(w, secretKey, durationMs);
  if (w.overflowed()) return Result::TooLarge;
  RecordMeta m;
  m.topic = OutTopic::GwClaim;
  m.cls = RecordClass::Control;
  m.dev = static_cast<uint16_t>(index);
  return client_->enqueue(m, reinterpret_cast<const uint8_t*>(buf), w.size());
}

const char* GatewayBase::nameOf(uint16_t dev, void* ctx) {
  const GatewayBase* self = static_cast<const GatewayBase*>(ctx);
  return dev < self->count_ ? self->devices_[dev].name : nullptr;
}

// Detach here rather than in ~ApiModule: by then onDetach() would no longer
// dispatch to GatewayBase, and the client would keep a resolver into a
// destroyed table.
GatewayBase::~GatewayBase() {
  if (client_) client_->detach(*this);
}

void GatewayBase::attach(Client& client) {
  ApiModule::attach(client);
  client.setDeviceNames(nameOf, this);
}

void GatewayBase::onDetach() {
  // Records still queued for sub-devices cannot be addressed any more; the
  // drain discards them as dead letters.
  if (client_) client_->setDeviceNames(nullptr, nullptr);
  ApiModule::onDetach();
}

void GatewayBase::topics(TopicSink& sink) {
  sink.add(InTopic::GwRpc);
  sink.add(InTopic::GwAttrUpdate);
  sink.add(InTopic::GwAttrResponse);
}

bool GatewayBase::handle(const Topic& topic, Bytes payload) {
  switch (topic.kind) {
    case InTopic::GwRpc:
      serveRpc(payload);
      return true;
    case InTopic::GwAttrUpdate:
      dispatchUpdate(payload);
      return true;
    case InTopic::GwAttrResponse:
      resolveResponse(payload);
      return true;
    default:
      return false;
  }
}

void GatewayBase::serveRpc(Bytes payload) {
  JsonValue msg = JsonValue::parse(payload);
  JsonValue data = msg["data"];
  JsonValue id = data["id"];
  char device[VL_GW_NAME_SIZE];
  if (!msg["device"].copyString(device, sizeof(device)) || id.type() != JsonValue::Type::Number) {
    VL_LOGW("gateway: RPC without device or id dropped");
    return;
  }
  uint32_t rid = static_cast<uint32_t>(id.asInt());
  RpcReply reply(client_, device, rid, reply_, sizeof(reply_));
  JsonValue method = data["method"];
  if (method.type() != JsonValue::Type::String) {
    reply.error("bad request");
    return;
  }
  char name[VL_RPC_METHOD_SIZE];
  const Method* found = methods_.find(method, name);
  if (!found) {
    reply.unknownMethod(name);
    return;
  }
  RpcRequest request(rid, name, data["params"]);
  found->fn(device, request, reply, found->ctx);
  reply.finish();
}

Result GatewayBase::requestAt(size_t index, bool client, const char* keys, GwAttrResponseCb cb, void* ctx,
                              uint32_t timeoutMs) {
  void* handle = nullptr;
  uint32_t id = requests_.open(*client_, this, timeoutMs, onResponse, cb, ctx, handle);
  if (id == 0) return Result::NoMemory;
  char buf[VL_ATTR_KEYS_SIZE * 4 + 2 * VL_GW_NAME_SIZE];
  JsonWriter w(buf, sizeof(buf));
  codec::gwAttributeRequest(w, id, devices_[index].name, client, keys);
  if (w.overflowed()) {
    requests_.abort(*client_, id, handle);
    return Result::TooLarge;
  }
  RecordMeta m;
  m.topic = OutTopic::GwAttrRequest;
  m.cls = RecordClass::Control;
  return requests_.submit(*client_, id, handle, m, buf, w.size());
}

namespace {
struct UpdateVisit {
  GwAttrUpdateCb cb;
  void* ctx;
  const char* device;
};
}  // namespace

void GatewayBase::visitUpdate(const char* key, JsonValue value, void* ctx) {
  const UpdateVisit* v = static_cast<const UpdateVisit*>(ctx);
  v->cb(v->device, key, value, v->ctx);
}

void GatewayBase::dispatchUpdate(Bytes payload) {
  if (!attrCb_) return;
  JsonValue msg = JsonValue::parse(payload);
  char device[VL_GW_NAME_SIZE];
  UpdateVisit visit = {attrCb_, attrCtx_, device};
  if (!msg["device"].copyString(device, sizeof(device)) || !forEachAttribute(msg["data"], visitUpdate, &visit)) {
    VL_LOGW("gateway: malformed attribute update dropped");
  }
}

void GatewayBase::resolveResponse(Bytes payload) {
  // The request id travels in the payload, not in the topic.
  JsonValue id = JsonValue::parse(payload)["id"];
  if (id.type() != JsonValue::Type::Number ||
      !client_->resolvePending(static_cast<uint32_t>(id.asInt()), payload.data(), payload.size())) {
    VL_LOGW("gateway: attribute response for no pending request");
  }
}

void GatewayBase::onResponse(Result r, const uint8_t* p, size_t n, void* ctx) {
  GwAttrResponseCb cb;
  void* userCtx;
  RequestSlots<GwAttrResponseCb, VL_GW_MAX_REQUESTS>::finish(ctx, cb, userCtx);
  if (!cb) return;
  JsonValue response;
  if (r == Result::Ok) {
    response = JsonValue::parse(reinterpret_cast<const char*>(p), n);
    if (response.type() != JsonValue::Type::Object) {
      response = JsonValue();
      r = Result::Rejected;
    }
  }
  cb(r, response, userCtx);
}

void GatewayBase::onOnline() {
  // Clean session: the server has forgotten which sub-devices this gateway
  // serves. Priority puts the connect events ahead of their queued data.
  size_t n;
  {
    LockGuard g(client_->lock());
    n = count_;
  }
  for (size_t i = 0; i < n; ++i) {
    bool connected;
    {
      LockGuard g(client_->lock());
      connected = devices_[i].connected;
    }
    if (!connected) continue;
    Result r = queueConnect(i, RecordClass::Priority);
    if (!ok(r)) VL_LOGW("gateway: connect for %s not replayed: %s", devices_[i].name, toString(r));
  }
}

}  // namespace vl

#endif  // VL_ENABLE_GATEWAY
