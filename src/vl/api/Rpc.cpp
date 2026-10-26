#include "vl/api/Rpc.h"

#include <stdio.h>
#include <string.h>

#include "vl/Client.h"
#include "vl/core/Codec.h"

namespace vl {

Result RpcReply::publish(const char* json, size_t n) {
  if (!client_) return Result::NotReady;
  RecordMeta m;
  m.cls = RecordClass::Priority;
  Result r;
  if (device_) {
#if VL_ENABLE_GATEWAY
    char out[VL_RPC_REPLY_SIZE + 2 * VL_GW_NAME_SIZE + 32];
    JsonWriter w(out, sizeof(out));
    codec::gwRpcReply(w, device_, id_, json, n);
    m.topic = OutTopic::GwRpc;
    r = w.overflowed() ? Result::TooLarge : client_->enqueue(m, reinterpret_cast<const uint8_t*>(out), w.size());
#else
    r = Result::Rejected;
#endif
  } else {
    m.topic = OutTopic::RpcResponse;
    m.arg = id_;
    r = client_->enqueue(m, reinterpret_cast<const uint8_t*>(json), n);
  }
  if (r == Result::TooLarge) tooLarge_ = true;
  if (ok(r)) sent_ = true;
  return r;
}

Result RpcReply::send() {
  if (sent_) return Result::Rejected;
  if (w_.size() == 0) return publish("{}", 2);
  if (w_.overflowed()) {
    tooLarge_ = true;
    return Result::TooLarge;
  }
  if (!w_.complete()) return Result::Rejected;
  return publish(w_.c_str(), w_.size());
}

Result RpcReply::send(const char* json) {
  if (sent_) return Result::Rejected;
  // The server closes the session on an invalid payload: never publish one.
  if (!json || !JsonValue::parse(json, strlen(json)).valid()) return Result::Rejected;
  return publish(json, strlen(json));
}

Result RpcReply::error(const char* message) {
  if (sent_) return Result::Rejected;
  JsonWriter w(buf_, cap_);
  codec::rpcError(w, message ? message : "error");
  if (w.overflowed()) {
    tooLarge_ = true;
    return Result::TooLarge;
  }
  return publish(w.c_str(), w.size());
}

void RpcReply::unknownMethod(const char* name) {
  char msg[VL_RPC_METHOD_SIZE + 16] = "unknown method";
  if (name[0]) snprintf(msg, sizeof(msg), "unknown method: %s", name);
  error(msg);
}

void RpcReply::finish() {
  if (!sent_) error(tooLarge_ ? "reply too large" : "no reply");
}

Result RpcBase::on(const char* method, RpcHandler fn, void* ctx) { return methods_.add(method, fn, ctx); }

void RpcBase::topics(TopicSink& sink) {
  sink.add(InTopic::RpcRequest);
  sink.add(InTopic::RpcResponse);
}

bool RpcBase::handle(const Topic& topic, Bytes payload) {
  if (topic.kind == InTopic::RpcResponse) return client_->resolvePending(topic.id, payload.data(), payload.size());
  if (topic.kind != InTopic::RpcRequest) return false;
  serve(topic.id, payload);
  return true;
}

Result RpcBase::call(const char* method, const char* paramsJson, RpcResultCb cb, void* ctx, uint32_t timeoutMs) {
  if (!method || !method[0]) return Result::Rejected;
  size_t paramsLen = paramsJson ? strlen(paramsJson) : 0;
  if (paramsJson && !JsonValue::parse(paramsJson, paramsLen).valid()) return Result::Rejected;
  if (!client_) return Result::NotReady;
  char buf[VL_RPC_REPLY_SIZE];
  JsonWriter w(buf, sizeof(buf));
  codec::rpcRequest(w, method, paramsJson, paramsLen);
  if (w.overflowed()) return Result::TooLarge;
  RecordMeta m;
  m.topic = OutTopic::RpcRequest;
  m.cls = RecordClass::Control;
  return calls_.start(*client_, this, m, buf, w.size(), timeoutMs, onCallDone, cb, ctx);
}

void RpcBase::onCallDone(Result r, const uint8_t* p, size_t n, void* ctx) {
  RpcResultCb cb;
  void* userCtx;
  RequestSlots<RpcResultCb, VL_RPC_MAX_CALLS>::finish(ctx, cb, userCtx);
  if (!cb) return;
  JsonValue response;
  if (r == Result::Ok) {
    response = JsonValue::parse(reinterpret_cast<const char*>(p), n);
    if (!response.valid()) r = Result::Rejected;
  }
  cb(r, response, userCtx);
}

void RpcBase::serve(uint32_t id, Bytes payload) {
  RpcReply reply(client_, id, reply_, sizeof(reply_));
  JsonValue req = JsonValue::parse(payload);
  JsonValue method = req["method"];
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
  RpcRequest request(id, name, req["params"]);
  found->fn(request, reply, found->ctx);
  reply.finish();
}

}  // namespace vl
