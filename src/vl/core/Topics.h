// ThingsBoard MQTT v1 device/gateway topics and v2 firmware topics. Outgoing
// topics are stored in the outbox as a small enum plus numeric arguments, never
// as strings.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vl {

enum class OutTopic : uint8_t {
  Telemetry,         // v1/devices/me/telemetry
  Attributes,        // v1/devices/me/attributes
  AttrRequest,       // v1/devices/me/attributes/request/{arg}
  RpcResponse,       // v1/devices/me/rpc/response/{arg}
  RpcRequest,        // v1/devices/me/rpc/request/{arg}
  Claim,             // v1/devices/me/claim
  ProvisionRequest,  // /provision/request
  FwChunkRequest,    // v2/fw/request/{arg}/chunk/{arg2}
  GwConnect,         // v1/gateway/connect
  GwDisconnect,      // v1/gateway/disconnect
  GwTelemetry,       // v1/gateway/telemetry
  GwAttributes,      // v1/gateway/attributes
  GwAttrRequest,     // v1/gateway/attributes/request
  GwRpc,             // v1/gateway/rpc
  GwClaim,           // v1/gateway/claim
  Count
};

// Writes the topic string; returns its length, or 0 if it did not fit.
size_t formatTopic(OutTopic t, uint32_t arg, uint32_t arg2, char* buf, size_t cap);

enum class InTopic : uint8_t {
  Unknown,
  AttrUpdate,         // v1/devices/me/attributes
  AttrResponse,       // v1/devices/me/attributes/response/{id}
  RpcRequest,         // v1/devices/me/rpc/request/{id}   (server-side RPC)
  RpcResponse,        // v1/devices/me/rpc/response/{id}  (client-side RPC reply)
  FwChunk,            // v2/fw/response/{id}/chunk/{id2}
  GwRpc,              // v1/gateway/rpc
  GwAttrUpdate,       // v1/gateway/attributes
  GwAttrResponse,     // v1/gateway/attributes/response
  ProvisionResponse,  // /provision/response
  Count
};

struct Topic {
  InTopic kind = InTopic::Unknown;
  uint32_t id = 0;
  uint32_t id2 = 0;
  const char* raw = nullptr;
};

Topic parseTopic(const char* topic);

// Subscription filter for an incoming topic kind.
const char* filterFor(InTopic kind);

}  // namespace vl
