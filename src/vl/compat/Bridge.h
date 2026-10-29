// The platform-free half of the 1.0.2 shim (ViraLinkCompat.h): 1.0.2 topic
// strings and raw JSON payloads in, v2 outbox records out, and incoming
// messages handed to one callback the way 1.0.2 delivered them.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vl/Config.h"
#include "vl/IApiModule.h"
#include "vl/Result.h"
#include "vl/core/Outbox.h"

namespace vl {

#if VL_ENABLE_GATEWAY
class GatewayBase;
#endif

namespace compat {

// An incoming message as 1.0.2 delivered it. topic and payload are valid
// during the call. Return true if handled.
typedef bool (*MessageCb)(const char* topic, const uint8_t* payload, size_t len, void* ctx);

// Maps the 1.0.2 MQTTController calls onto a v2 client:
//  - device telemetry is stamped like client.telemetry(), unless it carries
//    its own {"ts":…,"values":…}; an array queues one record per element;
//  - RPC replies jump the queue, claims are never saved to a store;
//  - RPC requests, shared-attribute updates and attribute responses nobody
//    is waiting for go to the message callback; with a gateway, so do
//    sub-device RPC and attribute updates.
// Attach it before other modules that read attribute responses.
class Bridge : public ApiModule {
 public:
  // Call before begin().
  void onMessage(MessageCb cb, void* ctx) {
    cb_ = cb;
    ctx_ = ctx;
  }
#if VL_ENABLE_GATEWAY
  // Sends sub-device data through gw and takes v1/gateway/rpc and
  // v1/gateway/attributes. Attach gw after the bridge. Call before
  // client.use(bridge).
  void setGateway(GatewayBase& gw) { gw_ = &gw; }
#endif

  // Queues payload on a 1.0.2 topic: device telemetry, attributes, claim,
  // attribute request, RPC request or reply, and gateway connect, disconnect,
  // attribute request or RPC reply. Rejected for any other topic, gateway
  // telemetry, attributes and claims included (use telemetry(device, …)), and
  // for a payload the topic cannot take. Thread-safe.
  Result publish(const char* topic, const char* payload, size_t len);
  // device null or "": this device. A sub-device is connected first, as
  // gw.device() does; NotReady without a gateway. Thread-safe.
  Result telemetry(const char* device, const char* json, size_t len);
  Result attributes(const char* device, const char* json, size_t len);
  Result claim(const char* device, const char* secretKey, uint32_t durationMs);
  Result connectDevice(const char* device);
  Result disconnectDevice(const char* device);
  // Publishes keysJson (e.g. {"sharedKeys":"a,b"}) as an attribute request;
  // cb gets the response as a message. Like 1.0.2, a request that times out
  // is dropped silently. NoMemory when VL_COMPAT_MAX_REQUESTS are
  // outstanding. Thread-safe.
  Result requestAttributes(const char* keysJson, size_t len, MessageCb cb, void* ctx,
                           uint32_t timeoutMs = VL_ATTR_TIMEOUT_MS);

  void topics(TopicSink& sink) override;
  bool handle(const Topic& topic, Bytes payload) override;
  void onDetach() override;

 private:
  struct Request {
    Bridge* self;
    uint32_t id;
    MessageCb cb;
    void* ctx;
    bool used;
  };

  Result values(OutTopic topic, uint16_t dev, bool stamp, const char* json, size_t len);
  Result subDevice(const char* device, uint16_t& index);
  Result raw(OutTopic topic, RecordClass cls, uint32_t arg, const char* payload, size_t len);
  bool forward(const char* topic, Bytes payload);
  static void onResponse(Result r, const uint8_t* p, size_t n, void* ctx);

  MessageCb cb_ = nullptr;
  void* ctx_ = nullptr;
#if VL_ENABLE_GATEWAY
  GatewayBase* gw_ = nullptr;
#endif
  Request requests_[VL_COMPAT_MAX_REQUESTS] = {};
};

}  // namespace compat
}  // namespace vl
