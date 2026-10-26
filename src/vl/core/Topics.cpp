#include "vl/core/Topics.h"

#include <stdio.h>
#include <string.h>

namespace vl {

namespace {

const char* const kOutFixed[] = {
    "v1/devices/me/telemetry",
    "v1/devices/me/attributes",
    "v1/devices/me/attributes/request/",
    "v1/devices/me/rpc/response/",
    "v1/devices/me/rpc/request/",
    "v1/devices/me/claim",
    "/provision/request",
    "v2/fw/request/",
    "v1/gateway/connect",
    "v1/gateway/disconnect",
    "v1/gateway/telemetry",
    "v1/gateway/attributes",
    "v1/gateway/attributes/request",
    "v1/gateway/rpc",
    "v1/gateway/claim",
};
static_assert(sizeof(kOutFixed) / sizeof(kOutFixed[0]) == static_cast<size_t>(OutTopic::Count),
              "kOutFixed out of sync with OutTopic");

const char* const kFilters[] = {
    nullptr,
    "v1/devices/me/attributes",
    "v1/devices/me/attributes/response/+",
    "v1/devices/me/rpc/request/+",
    "v1/devices/me/rpc/response/+",
    "v2/fw/response/+/chunk/+",
    "v1/gateway/rpc",
    "v1/gateway/attributes",
    "v1/gateway/attributes/response",
    "/provision/response",
};
static_assert(sizeof(kFilters) / sizeof(kFilters[0]) == static_cast<size_t>(InTopic::Count),
              "kFilters out of sync with InTopic");

// Parses a decimal id that must run to the end of the string or to '/'.
bool parseId(const char*& s, uint32_t& out) {
  if (*s < '0' || *s > '9') return false;
  uint32_t v = 0;
  while (*s >= '0' && *s <= '9') {
    v = v * 10u + static_cast<uint32_t>(*s - '0');
    ++s;
  }
  out = v;
  return true;
}

bool startsWith(const char* s, const char* prefix, const char*& rest) {
  size_t n = strlen(prefix);
  if (strncmp(s, prefix, n) != 0) return false;
  rest = s + n;
  return true;
}

}  // namespace

size_t formatTopic(OutTopic t, uint32_t arg, uint32_t arg2, char* buf, size_t cap) {
  if (t >= OutTopic::Count) return 0;
  const char* base = kOutFixed[static_cast<size_t>(t)];
  int n;
  switch (t) {
    case OutTopic::AttrRequest:
    case OutTopic::RpcResponse:
    case OutTopic::RpcRequest:
      n = snprintf(buf, cap, "%s%lu", base, static_cast<unsigned long>(arg));
      break;
    case OutTopic::FwChunkRequest:
      n = snprintf(buf, cap, "%s%lu/chunk/%lu", base, static_cast<unsigned long>(arg),
                   static_cast<unsigned long>(arg2));
      break;
    default:
      n = snprintf(buf, cap, "%s", base);
      break;
  }
  if (n < 0 || static_cast<size_t>(n) >= cap) return 0;
  return static_cast<size_t>(n);
}

Topic parseTopic(const char* s) {
  Topic t;
  t.raw = s;
  const char* r;
  uint32_t id;
  if (strcmp(s, "v1/devices/me/attributes") == 0) {
    t.kind = InTopic::AttrUpdate;
  } else if (startsWith(s, "v1/devices/me/attributes/response/", r)) {
    if (parseId(r, id) && *r == '\0') {
      t.kind = InTopic::AttrResponse;
      t.id = id;
    }
  } else if (startsWith(s, "v1/devices/me/rpc/request/", r)) {
    if (parseId(r, id) && *r == '\0') {
      t.kind = InTopic::RpcRequest;
      t.id = id;
    }
  } else if (startsWith(s, "v1/devices/me/rpc/response/", r)) {
    if (parseId(r, id) && *r == '\0') {
      t.kind = InTopic::RpcResponse;
      t.id = id;
    }
  } else if (startsWith(s, "v2/fw/response/", r)) {
    const char* r2;
    uint32_t chunk;
    if (parseId(r, id) && startsWith(r, "/chunk/", r2) && parseId(r2, chunk) && *r2 == '\0') {
      t.kind = InTopic::FwChunk;
      t.id = id;
      t.id2 = chunk;
    }
  } else if (strcmp(s, "v1/gateway/rpc") == 0) {
    t.kind = InTopic::GwRpc;
  } else if (strcmp(s, "v1/gateway/attributes") == 0) {
    t.kind = InTopic::GwAttrUpdate;
  } else if (strcmp(s, "v1/gateway/attributes/response") == 0) {
    t.kind = InTopic::GwAttrResponse;
  } else if (strcmp(s, "/provision/response") == 0) {
    t.kind = InTopic::ProvisionResponse;
  }
  return t;
}

const char* filterFor(InTopic kind) {
  if (kind >= InTopic::Count) return nullptr;
  return kFilters[static_cast<size_t>(kind)];
}

bool expectsSuback(InTopic kind) { return kind != InTopic::ProvisionResponse; }

}  // namespace vl
