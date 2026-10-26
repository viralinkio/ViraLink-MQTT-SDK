#pragma once

#include "vl/Config.h"

#if VL_ENABLE_GATEWAY

#include <stddef.h>
#include <stdint.h>

#include "vl/Client.h"
#include "vl/IApiModule.h"
#include "vl/Payload.h"
#include "vl/Result.h"
#include "vl/api/Attributes.h"
#include "vl/api/MethodTable.h"
#include "vl/api/Requests.h"
#include "vl/api/Rpc.h"

namespace vl {

class GatewayBase;

// Server-side RPC addressed to a sub-device. device is valid during the call.
typedef void (*GwRpcHandler)(const char* device, const RpcRequest& req, RpcReply& reply, void* ctx);
// A shared attribute of a sub-device changed; value is Invalid when it was
// deleted. device and value are valid during the call.
typedef void (*GwAttrUpdateCb)(const char* device, const char* key, JsonValue value, void* ctx);
// Outcome of a sub-device attribute request. When r is Ok, response is the
// server's object: {"id":…,"device":…,"values":{…}}, or "value":… instead of
// "values" when exactly one attribute was found. Invalid otherwise (Timeout,
// NotReady after end(), Rejected for a malformed reply).
typedef void (*GwAttrResponseCb)(Result r, JsonValue response, void* ctx);

// A sub-device of the gateway: a table index, valid while the gateway lives.
// Cheap to copy. An invalid handle (status() != Ok) refuses every send with
// that status.
class SubDevice {
 public:
  bool valid() const { return status_ == Result::Ok; }
  Result status() const { return status_; }
  const char* name() const;

  // Builders as on Client: gw.device("Meter-12").telemetry().add("kwh", 3.5).send().
  template <size_t N = VL_BUILDER_SIZE>
  PayloadBuilder<N> telemetry() const;
  template <size_t N = VL_BUILDER_SIZE>
  PayloadBuilder<N> attributes() const;
  // Lets a user claim this sub-device with secretKey (null = none) within durationMs.
  Result claim(const char* secretKey, uint32_t durationMs) const;
  Result disconnect() const;
  // Requests client (client = true) or shared attributes of this sub-device.
  // keys is comma-separated. cb fires once with the response or Timeout.
  Result requestAttributes(bool client, const char* keys, GwAttrResponseCb cb, void* ctx,
                           uint32_t timeoutMs = VL_ATTR_TIMEOUT_MS) const;

 private:
  friend class GatewayBase;
  SubDevice(GatewayBase* gw, uint16_t index, Result status) : gw_(gw), index_(index), status_(status) {}
  // NotReady once the gateway left the client.
  Result usable() const;

  GatewayBase* gw_;
  uint16_t index_;
  Result status_;
};

// Gateway API: one MQTT session speaking for several sub-devices. Use
// Gateway<N>; at most one per client.
class GatewayBase : public ApiModule {
 public:
  // Returns the sub-device called name, adding it if new, and queues a
  // connect event unless it is already connected. type (null = server
  // default) is used when the device is added. Names and types are copied.
  // NoMemory when the table is full, Rejected for an empty or too long name
  // or type, NotReady before client.use(). Thread-safe.
  SubDevice device(const char* name, const char* type = nullptr);
  // The sub-device called name without connecting it; Rejected if unknown.
  SubDevice find(const char* name);
  // Queues a disconnect event if name is connected (Queued), else does
  // nothing (Ok). The table slot is kept, so records already queued for it
  // are still delivered. Thread-safe.
  Result disconnect(const char* name);
  // Registers or replaces the handler for a sub-device RPC method; the name
  // is copied. Every request gets a reply, as with Rpc. Call before begin().
  Result on(const char* method, GwRpcHandler fn, void* ctx = nullptr) { return methods_.add(method, fn, ctx); }
  // Fires cb for every updated or deleted shared attribute of any
  // sub-device. Call before begin().
  void onAttributeUpdate(GwAttrUpdateCb cb, void* ctx = nullptr) {
    attrCb_ = cb;
    attrCtx_ = ctx;
  }

  ~GatewayBase() override;
  void attach(Client& client) override;
  void onDetach() override;
  void topics(TopicSink& sink) override;
  bool handle(const Topic& topic, Bytes payload) override;
  void onOnline() override;

 protected:
  struct Device {
    char name[VL_GW_NAME_SIZE];
    char type[VL_GW_TYPE_SIZE];
    bool connected;
  };
  typedef MethodTable<GwRpcHandler>::Entry Method;
  GatewayBase(Device* devices, size_t cap, Method* methods, size_t methodCap)
      : devices_(devices), cap_(cap), methods_(methods, methodCap) {}

 private:
  friend class SubDevice;
  // Index of name, or count_ if absent. Call with the client lock held.
  size_t lookup(const char* name) const;
  Result queueConnect(size_t index, RecordClass cls);
  Result disconnectAt(size_t index);
  Result claimAt(size_t index, const char* secretKey, uint32_t durationMs);
  // Device-name resolver for the drain; runs with the client lock held and
  // only reads the table.
  static const char* nameOf(uint16_t dev, void* ctx);
  void serveRpc(Bytes payload);
  Result requestAt(size_t index, bool client, const char* keys, GwAttrResponseCb cb, void* ctx, uint32_t timeoutMs);
  void dispatchUpdate(Bytes payload);
  void resolveResponse(Bytes payload);
  static void visitUpdate(const char* key, JsonValue value, void* ctx);
  static void onResponse(Result r, const uint8_t* p, size_t n, void* ctx);

  Device* devices_;
  size_t cap_;
  size_t count_ = 0;
  MethodTable<GwRpcHandler> methods_;
  char reply_[VL_RPC_REPLY_SIZE];
  GwAttrUpdateCb attrCb_ = nullptr;
  void* attrCtx_ = nullptr;
  RequestSlots<GwAttrResponseCb, VL_GW_MAX_REQUESTS> requests_;
};

// Gateway with room for N sub-devices and Methods server-side RPC methods.
template <size_t N, size_t Methods = 4>
class Gateway : public GatewayBase {
  static_assert(N > 0 && N < kNoDevice, "sub-device index must fit RecordMeta::dev");

 public:
  Gateway() : GatewayBase(devices_, N, methods_, Methods) {}

 private:
  Device devices_[N];
  Method methods_[Methods];
};

template <size_t N>
PayloadBuilder<N> SubDevice::telemetry() const {
  Result r = usable();
  if (r != Result::Ok) return PayloadBuilder<N>(r);
  return PayloadBuilder<N>(*gw_->client_, OutTopic::GwTelemetry, index_, true);
}

template <size_t N>
PayloadBuilder<N> SubDevice::attributes() const {
  Result r = usable();
  if (r != Result::Ok) return PayloadBuilder<N>(r);
  return PayloadBuilder<N>(*gw_->client_, OutTopic::GwAttributes, index_, false);
}

}  // namespace vl

#endif  // VL_ENABLE_GATEWAY
