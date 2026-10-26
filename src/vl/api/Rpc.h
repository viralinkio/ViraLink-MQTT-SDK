#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vl/Config.h"
#include "vl/IApiModule.h"
#include "vl/Result.h"
#include "vl/api/MethodTable.h"
#include "vl/api/Requests.h"
#include "vl/core/JsonReader.h"
#include "vl/core/JsonWriter.h"

namespace vl {

class Client;

// One server-side RPC; valid only during its handler.
class RpcRequest {
 public:
  uint32_t id() const { return id_; }
  const char* method() const { return method_; }
  JsonValue params() const { return params_; }

 private:
  friend class RpcBase;
  friend class GatewayBase;
  RpcRequest(uint32_t id, const char* method, JsonValue params) : id_(id), method_(method), params_(params) {}

  uint32_t id_;
  const char* method_;
  JsonValue params_;
};

// The answer to one server-side RPC, published ahead of queued data. A
// handler that sends nothing gets {"error":"no reply"}, so the server never
// waits out its RPC timeout.
class RpcReply {
 public:
  // Writer for the reply body; send() publishes what was written.
  JsonWriter& json() { return w_; }
  // Publishes json(), or {} if nothing was written. Rejected once sent.
  Result send();
  // Publishes pre-encoded JSON; Rejected if it does not parse.
  Result send(const char* json);
  // Publishes {"error":message}.
  Result error(const char* message);
  bool sent() const { return sent_; }

 private:
  friend class RpcBase;
  friend class GatewayBase;
  RpcReply(Client* c, uint32_t id, char* buf, size_t cap) : RpcReply(c, nullptr, id, buf, cap) {}
  // device != null: a sub-device RPC answered on v1/gateway/rpc. device must
  // outlive the reply.
  RpcReply(Client* c, const char* device, uint32_t id, char* buf, size_t cap)
      : client_(c), device_(device), id_(id), buf_(buf), cap_(cap), w_(buf, cap) {}
  Result publish(const char* json, size_t n);
  // Automatic replies, so the server never waits out its RPC timeout.
  void unknownMethod(const char* name);
  void finish();

  Client* client_;
  const char* device_;
  uint32_t id_;
  char* buf_;
  size_t cap_;
  JsonWriter w_;
  bool sent_ = false;
  bool tooLarge_ = false;
};

typedef void (*RpcHandler)(const RpcRequest& req, RpcReply& reply, void* ctx);

// Outcome of a client-side RPC. response is the server's JSON when r is Ok,
// Invalid otherwise (Timeout, NotReady after end(), Rejected for non-JSON).
typedef void (*RpcResultCb)(Result r, JsonValue response, void* ctx);

// Server-side RPC through a fixed method registry, where every request gets
// a reply (unknown methods included), and client-side calls. Use Rpc<N>.
class RpcBase : public ApiModule {
 public:
  // Registers or replaces the handler for method; the name is copied.
  // Call before begin().
  Result on(const char* method, RpcHandler fn, void* ctx = nullptr);

  // Publishes {"method":…,"params":…} and later fires cb exactly once with
  // the response or Timeout. paramsJson may be null. NoMemory when
  // VL_RPC_MAX_CALLS calls are outstanding. Thread-safe.
  Result call(const char* method, const char* paramsJson, RpcResultCb cb, void* ctx,
              uint32_t timeoutMs = VL_RPC_TIMEOUT_MS);

  void topics(TopicSink& sink) override;
  bool handle(const Topic& topic, Bytes payload) override;

 protected:
  typedef MethodTable<RpcHandler>::Entry Method;
  RpcBase(Method* table, size_t cap) : methods_(table, cap) {}

 private:
  void serve(uint32_t id, Bytes payload);
  static void onCallDone(Result r, const uint8_t* p, size_t n, void* ctx);

  MethodTable<RpcHandler> methods_;
  RequestSlots<RpcResultCb, VL_RPC_MAX_CALLS> calls_;
  char reply_[VL_RPC_REPLY_SIZE];
};

// RPC module with room for N server-side methods.
template <size_t N>
class Rpc : public RpcBase {
 public:
  Rpc() : RpcBase(entries_, N) {}

 private:
  Method entries_[N];
};

}  // namespace vl
