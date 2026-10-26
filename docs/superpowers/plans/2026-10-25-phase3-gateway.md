# Phase 3: Gateway Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `Gateway<N>` module that lets one MQTT session speak for N sub-devices: connect and disconnect,
telemetry, attributes, claim, server RPC, attribute updates and attribute requests, with connect events
replayed after every reconnect. All of it is heap-free and tested on the host.

**Architecture:** `GatewayBase` is an `ApiModule` (`src/vl/api/Gateway.h/.cpp`) that owns a fixed table of sub-device
names. Sub-device records share the client outbox and carry the table index in `RecordMeta::dev`. At publish time the
drain resolves that index to a name through `Client::setDeviceNames`, which the gateway registers when it attaches. The
user reaches a sub-device through a small `SubDevice` handle. Everything the server sends is consumed by functions the
user registers: `gw.on(method, fn)` for RPC and `gw.onAttributeUpdate(fn)` for updates, plus a callback per attribute
request.

**Tech Stack:** C++11 (`-std=gnu++11`), PlatformIO `native` + Unity, ASan/UBSan, `-Werror`.

## Global Constraints

- C++11 in `src/`; no heap, `std::string`, `std::function` or exceptions after `begin()`.
- Capacities are template parameters or `VL_*` macros in `Config.h`; full tables return `Result::NoMemory`.
- Callbacks are function pointers plus `void* ctx`, fired on the client task with no lock held.
- Hold the client lock only around outbox and table access: never during network I/O, never across a user callback.
- Every payload shape lives in `core/Codec` with a golden-string test (the gateway shapes already exist there).
- Strings passed in by the caller are copied if kept beyond the call.
- RPC replies use `RecordClass::Priority`; connect, disconnect, claim and requests use `RecordClass::Control`.
- Every server RPC that names a device and an id gets a reply, including unknown methods and unanswered handlers.
- The gateway compiles only when `VL_ENABLE_GATEWAY` is set (off in the Lite profile, so AVR never builds it).
- Portable to every non-Lite target: Cortex-M (newlib-nano: no float printf), ESP8266, ESP32 (Xtensa and RISC-V). In
  `src/`: no C++ standard library headers outside `platform/`, no `printf` of floats or `%ll`/`%z`, no identifiers that
  Arduino.h defines as macros (`min`, `max`, `abs`, `round`, `bit`, `DEC`, `HEX`, …).
- Gate: `pio test -e native`, `scripts/check_portability.sh` and `scripts/build_examples.sh` pass.
- Commits: only when the user asks (the repository has no commits yet).

## Wire format (ThingsBoard gateway API, v1 JSON topics)

| Direction | Topic | Payload |
| --- | --- | --- |
| out | `v1/gateway/connect` | `{"device":"Meter-12","type":"energy-meter"}` (`type` omitted when empty) |
| out | `v1/gateway/disconnect` | `{"device":"Meter-12"}` |
| out | `v1/gateway/telemetry` | `{"Meter-12":[{"ts":T,"values":{…}}]}`, or `{"Meter-12":[{…}]}` with no UTC |
| out | `v1/gateway/attributes` | `{"Meter-12":{…}}` |
| out | `v1/gateway/claim` | `{"Meter-12":{"secretKey":"…","durationMs":60000}}` |
| out | `v1/gateway/attributes/request` | `{"id":7,"device":"Meter-12","client":true,"keys":["fw","model"]}` |
| in | `v1/gateway/attributes/response` | `{"id":7,"device":"Meter-12","values":{…}}`, or `"value":…` when exactly one attribute was found |
| in | `v1/gateway/attributes` | `{"device":"Meter-12","data":{"mode":"eco","deleted":["old"]}}` |
| in | `v1/gateway/rpc` | `{"device":"Meter-12","data":{"id":5,"method":"setValve","params":{…}}}` |
| out | `v1/gateway/rpc` | `{"device":"Meter-12","id":5,"data":{…}}` |

The `value`/`values` split follows ThingsBoard's `JsonConverter.getJsonObjectForGateway`, which picks the form by the
number of attributes found. The Phase 6 integration suite confirms it against a live server.

## Decisions

- **Sub-device slots are never freed.** `disconnect` clears the connected flag but keeps the name, so records already
  queued for that index still resolve at publish time. The capacity N is therefore the number of distinct sub-devices
  per boot. A removable table would need generation counters in every record; nothing needs that yet.
- **Replay with `RecordClass::Priority`.** On every Online the gateway queues a connect event for each sub-device marked
  connected. Priority places these events ahead of queued data, so after an outage a device's connect still precedes
  its backlog. A device connected while offline may send its connect event twice; ThingsBoard treats a repeat connect
  as a no-op.
- **Callback registration only.** RPC goes through a method registry (`gw.on`), like `Rpc<N>`, whose handlers also get
  the device name. Attribute updates go to one `onAttributeUpdate` callback that gets the device, key and value. There
  is no binding helper for sub-devices.
- **Attribute responses are passed through whole.** The callback gets the response object and reads `values` or
  `value`. Normalizing them would mean storing the requested keys per request.
- **One gateway per client.** It owns the client's device-name resolver.
- **First-use race accepted.** `device()` is thread-safe, but if two tasks use a new name at the same moment, one task's
  data can be queued before the other's connect event. ThingsBoard creates and connects a sub-device on its first data
  anyway, so this costs nothing.
- **Not in this phase:** coalescing consecutive telemetry into one array publish (a drain feature for device and
  gateway records alike), sub-device OTA, and gateway RPC calls from device to server.

## File Structure

| File | Responsibility |
| --- | --- |
| `src/vl/api/MethodTable.h` | `MethodTable<Fn>`: fixed RPC method registry shared by `Rpc` and `Gateway` |
| `src/vl/api/Rpc.h/.cpp` | `RpcBase` on `MethodTable`; `RpcReply` gains a gateway mode and its automatic replies |
| `src/vl/api/Requests.h` | `RequestSlots` gains `open`/`submit`/`abort` for payloads that embed the request id |
| `src/vl/api/Attributes.h/.cpp` | `forEachAttribute` extracted for reuse by the gateway |
| `src/vl/api/Gateway.h/.cpp` | `GatewayBase`, `Gateway<N, Methods>`, `SubDevice` |
| `src/vl/Payload.h` | `PayloadBuilder(Result failed)` for handles that cannot send |
| `src/vl/Config.h` | `VL_GW_NAME_SIZE 32`, `VL_GW_TYPE_SIZE 24`, `VL_GW_MAX_REQUESTS 4` |
| `src/ViraLink.h` | include `vl/api/Gateway.h` |
| `test/test_gateway/test_main.cpp` | new suite |
| `test/portability/portability.ino` | gateway use under `#if VL_ENABLE_GATEWAY` |
| `examples/gateway/gateway.ino` | Arduino example |

---

### Task 1: Shared method registry and RPC automatic replies

A pure refactor so that Task 3 can reuse the method lookup and the automatic error replies. The existing
`test_rpc` suite is the test.

**Files:**
- Create: `src/vl/api/MethodTable.h`
- Modify: `src/vl/api/Rpc.h`, `src/vl/api/Rpc.cpp`

**Interfaces:**
- Produces:

```cpp
template <class Fn> class MethodTable {
 public:
  struct Entry { char name[VL_RPC_METHOD_SIZE]; Fn fn; void* ctx; };
  MethodTable(Entry* table, size_t cap);
  Result add(const char* name, Fn fn, void* ctx);
  const Entry* find(JsonValue method, char* name) const;  // name: VL_RPC_METHOD_SIZE bytes
};
// RpcReply, private, for RpcBase and GatewayBase:
void unknownMethod(const char* name);  // {"error":"unknown method: <name>"}, or "unknown method" if name is ""
void finish();                         // {"error":"no reply"} or {"error":"reply too large"} if nothing was sent
```

- [ ] **Step 1: Baseline.** Run `pio test -e native -f test_rpc`. Expected: all pass. This suite is the regression test
  for the refactor.

- [ ] **Step 2: Create `src/vl/api/MethodTable.h`**

```cpp
#pragma once

#include <stddef.h>
#include <string.h>

#include "vl/Config.h"
#include "vl/Result.h"
#include "vl/core/JsonReader.h"

namespace vl {

// Fixed registry of named RPC methods, shared by Rpc and Gateway.
template <class Fn>
class MethodTable {
 public:
  struct Entry {
    char name[VL_RPC_METHOD_SIZE];
    Fn fn;
    void* ctx;
  };

  MethodTable(Entry* table, size_t cap) : table_(table), cap_(cap) {}

  // Registers or replaces the handler for name; the name is copied.
  Result add(const char* name, Fn fn, void* ctx) {
    if (!name || !fn) return Result::Rejected;
    size_t n = strlen(name);
    if (n == 0 || n >= VL_RPC_METHOD_SIZE) return Result::Rejected;
    for (size_t i = 0; i < count_; ++i) {
      if (strcmp(table_[i].name, name) == 0) {
        table_[i].fn = fn;
        table_[i].ctx = ctx;
        return Result::Ok;
      }
    }
    if (count_ >= cap_) return Result::NoMemory;
    Entry& e = table_[count_++];
    memcpy(e.name, name, n + 1);
    e.fn = fn;
    e.ctx = ctx;
    return Result::Ok;
  }

  // Looks up the method named by a JSON string and copies the name into name
  // (VL_RPC_METHOD_SIZE bytes). A name that does not fit cannot be registered:
  // name is then "" and the result null.
  const Entry* find(JsonValue method, char* name) const {
    if (!method.copyString(name, VL_RPC_METHOD_SIZE)) {
      name[0] = '\0';
      return nullptr;
    }
    for (size_t i = 0; i < count_; ++i) {
      if (strcmp(table_[i].name, name) == 0) return &table_[i];
    }
    return nullptr;
  }

 private:
  Entry* table_;
  size_t cap_;
  size_t count_ = 0;
};

}  // namespace vl
```

- [ ] **Step 3: Move `RpcBase` onto it.** In `src/vl/api/Rpc.h`, add `#include "vl/api/MethodTable.h"`. In `RpcReply`,
  add the friend and the two private helpers:

```cpp
 private:
  friend class RpcBase;
  friend class GatewayBase;
  RpcReply(Client* c, uint32_t id, char* buf, size_t cap) : client_(c), id_(id), buf_(buf), cap_(cap), w_(buf, cap) {}
  Result publish(const char* json, size_t n);
  // Automatic replies, so the server never waits out its RPC timeout.
  void unknownMethod(const char* name);
  void finish();
```

Add `friend class GatewayBase;` to `RpcRequest` too. In `RpcBase`, replace the `Method` struct and the
`table_`/`cap_`/`count_` members:

```cpp
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

template <size_t N>
class Rpc : public RpcBase {
 public:
  Rpc() : RpcBase(entries_, N) {}

 private:
  Method entries_[N];
};
```

- [ ] **Step 4: Update `src/vl/api/Rpc.cpp`.** `RpcBase::on` becomes `return methods_.add(method, fn, ctx);`. Add the
  helpers and shorten `serve`:

```cpp
void RpcReply::unknownMethod(const char* name) {
  char msg[VL_RPC_METHOD_SIZE + 16] = "unknown method";
  if (name[0]) snprintf(msg, sizeof(msg), "unknown method: %s", name);
  error(msg);
}

void RpcReply::finish() {
  if (!sent_) error(tooLarge_ ? "reply too large" : "no reply");
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
```

- [ ] **Step 5: Run.** `pio test -e native -f test_rpc`. Expected: PASS with the same count as Step 1. Then run
  `scripts/check_portability.sh`. Expected: exit 0.

---

### Task 2: Sub-device table, connect, data, claim and replay

**Files:**
- Create: `src/vl/api/Gateway.h`, `src/vl/api/Gateway.cpp`, `test/test_gateway/test_main.cpp`
- Modify: `src/vl/Config.h`, `src/vl/Payload.h`, `test/portability/portability.ino`

**Interfaces:**
- Consumes: `Client::setDeviceNames(DeviceNameFn, void*)`, `Client::enqueue`, `Client::lock()`,
  `codec::gwConnect`, `codec::gwDisconnect`, `codec::claim`, `codec::encodeRecord` (already wraps `GwTelemetry`,
  `GwAttributes` and `GwClaim` in the device name).
- Produces:

```cpp
// Config.h
#define VL_GW_NAME_SIZE 32    // sub-device name incl. NUL
#define VL_GW_TYPE_SIZE 24    // device type (profile) incl. NUL
#define VL_GW_MAX_REQUESTS 4  // outstanding sub-device attribute requests

// Payload.h
explicit PayloadBuilder(Result failed);  // send() returns failed and queues nothing

class SubDevice {
 public:
  bool valid() const;
  Result status() const;     // Ok, or why the handle cannot send
  const char* name() const;  // "" when invalid
  template <size_t N = VL_BUILDER_SIZE> PayloadBuilder<N> telemetry() const;
  template <size_t N = VL_BUILDER_SIZE> PayloadBuilder<N> attributes() const;
  Result claim(const char* secretKey, uint32_t durationMs) const;
  Result disconnect() const;
};
class GatewayBase : public ApiModule {
 public:
  SubDevice device(const char* name, const char* type = nullptr);  // find or add, and connect
  SubDevice find(const char* name);                                 // Rejected if unknown
  Result disconnect(const char* name);
};
template <size_t N, size_t Methods = 4> class Gateway : public GatewayBase;
```

- [ ] **Step 1: Write the failing tests** (`test/test_gateway/test_main.cpp`)

```cpp
// Gateway: sub-device table, connect replay, data, claim, RPC and attributes.
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include <string>
#include <vector>

#include "ClientRig.h"
#include "vl/api/Gateway.h"

using namespace vl;
using vltest::ClientRig;
using vltest::Published;

void setUp() {}
void tearDown() {}

namespace {

struct GwRig : ClientRig {
  Gateway<2> gw;
  GwRig() { c.use(gw); }
  // Brings the session back Online after t.close().
  void reconnect() {
    clock.advance(2000);
    c.loop();
    t.connack(0);
    t.subackAll();
    TEST_ASSERT_EQUAL(State::Online, c.state());
  }
};

void expect(const Published& p, const char* topic, const char* payload) {
  TEST_ASSERT_EQUAL_STRING(topic, p.topic.c_str());
  TEST_ASSERT_EQUAL_STRING(payload, p.payload.c_str());
}

}  // namespace

static void test_subscribes_gateway_topics() {
  GwRig r;
  r.c.begin(r.cfg);
  r.c.loop();
  r.t.connack(0);
  bool rpc = false, upd = false, res = false;
  for (size_t i = 0; i < r.t.subscribed.size(); ++i) {
    rpc |= r.t.subscribed[i] == "v1/gateway/rpc";
    upd |= r.t.subscribed[i] == "v1/gateway/attributes";
    res |= r.t.subscribed[i] == "v1/gateway/attributes/response";
  }
  TEST_ASSERT_TRUE(rpc && upd && res);
}

static void test_connect_precedes_data() {
  GwRig r;
  r.connect();
  SubDevice m = r.gw.device("Meter-12", "energy-meter");
  TEST_ASSERT_TRUE(m.valid());
  TEST_ASSERT_EQUAL_STRING("Meter-12", m.name());
  TEST_ASSERT_EQUAL(Result::Ok, m.telemetry().add("kwh", 3.5).send());
  TEST_ASSERT_EQUAL(Result::Ok, m.telemetry().add("kwh", 3.75).ts(1700000000000ULL).send());
  TEST_ASSERT_EQUAL(Result::Ok, m.attributes().add("fw", "1.0").send());
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(4, p.size());
  expect(p[0], "v1/gateway/connect", "{\"device\":\"Meter-12\",\"type\":\"energy-meter\"}");
  expect(p[1], "v1/gateway/telemetry", "{\"Meter-12\":[{\"kwh\":3.5}]}");
  expect(p[2], "v1/gateway/telemetry", "{\"Meter-12\":[{\"ts\":1700000000000,\"values\":{\"kwh\":3.75}}]}");
  expect(p[3], "v1/gateway/attributes", "{\"Meter-12\":{\"fw\":\"1.0\"}}");
  TEST_ASSERT_EQUAL(Result::Ok, m.claim("secret", 60000));
  p = r.flush();
  TEST_ASSERT_EQUAL_UINT(1, p.size());
  expect(p[0], "v1/gateway/claim", "{\"Meter-12\":{\"secretKey\":\"secret\",\"durationMs\":60000}}");
}

static void test_connect_once_and_disconnect() {
  GwRig r;
  r.connect();
  r.gw.device("A");
  r.gw.device("A", "ignored");  // already connected: nothing queued
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(1, p.size());
  expect(p[0], "v1/gateway/connect", "{\"device\":\"A\"}");
  TEST_ASSERT_EQUAL(Result::Ok, r.gw.disconnect("A"));
  TEST_ASSERT_EQUAL(Result::Ok, r.gw.disconnect("A"));  // already disconnected: nothing queued
  TEST_ASSERT_EQUAL(Result::Rejected, r.gw.disconnect("nobody"));
  p = r.flush();
  TEST_ASSERT_EQUAL_UINT(1, p.size());
  expect(p[0], "v1/gateway/disconnect", "{\"device\":\"A\"}");
  TEST_ASSERT_TRUE(r.gw.find("A").valid());  // the slot stays
  r.gw.device("A");
  p = r.flush();
  TEST_ASSERT_EQUAL_UINT(1, p.size());
  expect(p[0], "v1/gateway/connect", "{\"device\":\"A\"}");
}

static void test_queued_records_keep_their_device() {
  GwRig r;
  r.connect();
  SubDevice a = r.gw.device("A");
  a.telemetry().add("n", 1).send();
  TEST_ASSERT_EQUAL(Result::Ok, a.disconnect());
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(3, p.size());
  expect(p[0], "v1/gateway/connect", "{\"device\":\"A\"}");
  expect(p[1], "v1/gateway/telemetry", "{\"A\":[{\"n\":1}]}");
  expect(p[2], "v1/gateway/disconnect", "{\"device\":\"A\"}");
}

static void test_replays_connected_devices_on_reconnect() {
  GwRig r;
  r.connect();
  SubDevice a = r.gw.device("A", "meter");
  r.gw.device("B");
  r.gw.disconnect("B");
  TEST_ASSERT_EQUAL_UINT(3, r.flush().size());
  r.t.close();
  a.telemetry().add("n", 1).send();  // queued while offline
  r.reconnect();
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(2, p.size());
  expect(p[0], "v1/gateway/connect", "{\"device\":\"A\",\"type\":\"meter\"}");
  expect(p[1], "v1/gateway/telemetry", "{\"A\":[{\"n\":1}]}");
}

static void test_table_capacity_and_validation() {
  GwRig r;
  TEST_ASSERT_TRUE(r.gw.device("a").valid());
  TEST_ASSERT_TRUE(r.gw.device("b").valid());
  SubDevice full = r.gw.device("c");
  TEST_ASSERT_EQUAL(Result::NoMemory, full.status());
  TEST_ASSERT_EQUAL(Result::NoMemory, full.telemetry().add("x", 1).send());
  TEST_ASSERT_EQUAL(Result::NoMemory, full.claim(nullptr, 1000));
  TEST_ASSERT_EQUAL_STRING("", full.name());
  TEST_ASSERT_EQUAL(Result::Rejected, r.gw.device(nullptr).status());
  TEST_ASSERT_EQUAL(Result::Rejected, r.gw.device("").status());
  std::string longName(VL_GW_NAME_SIZE, 'n');
  TEST_ASSERT_EQUAL(Result::Rejected, r.gw.device(longName.c_str()).status());
  std::string longType(VL_GW_TYPE_SIZE, 't');
  TEST_ASSERT_EQUAL(Result::Rejected, r.gw.device("a", longType.c_str()).status());
  TEST_ASSERT_EQUAL(Result::Rejected, r.gw.find("zz").status());
  Gateway<1> loose;  // never attached
  TEST_ASSERT_EQUAL(Result::NotReady, loose.device("a").status());
}

// Also guards the destructor order: without ~GatewayBase detaching, ASan
// reports the drain reading the destroyed gateway's table.
static void test_gateway_destroyed_with_queued_records() {
  ClientRig r;
  {
    Gateway<1> gw;
    r.c.use(gw);
    r.connect();
    gw.device("A").telemetry().add("n", 1).send();
  }
  // The connect event carries its own name; the telemetry can no longer be addressed.
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(1, p.size());
  expect(p[0], "v1/gateway/connect", "{\"device\":\"A\"}");
  TEST_ASSERT_EQUAL_UINT32(1, r.c.stats().deadLetters);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_subscribes_gateway_topics);
  RUN_TEST(test_connect_precedes_data);
  RUN_TEST(test_connect_once_and_disconnect);
  RUN_TEST(test_queued_records_keep_their_device);
  RUN_TEST(test_replays_connected_devices_on_reconnect);
  RUN_TEST(test_table_capacity_and_validation);
  RUN_TEST(test_gateway_destroyed_with_queued_records);
  return UNITY_END();
}
```

- [ ] **Step 2: Run, expect FAIL.** `pio test -e native -f test_gateway`. Expected: compile error,
  `vl/api/Gateway.h` not found.

- [ ] **Step 3: Config.** In `src/vl/Config.h`, after the attribute sizes:

```cpp
// Gateway
#ifndef VL_GW_NAME_SIZE
#define VL_GW_NAME_SIZE 32  // sub-device name, including the NUL
#endif
#ifndef VL_GW_TYPE_SIZE
#define VL_GW_TYPE_SIZE 24  // device type (profile name), including the NUL
#endif
#ifndef VL_GW_MAX_REQUESTS
#define VL_GW_MAX_REQUESTS 4  // outstanding sub-device attribute requests
#endif
```

- [ ] **Step 4: Failed builder.** In `src/vl/Payload.h`, add a `Result failed_` member and the constructor. Keep the
  member order (`hasTs_`, `ts_`, `failed_`, `buf_`, `w_`) so `-Wreorder` stays quiet:

```cpp
  // A builder whose send() returns failed and queues nothing, e.g. for a
  // sub-device the gateway could not add.
  explicit PayloadBuilder(Result failed)
      : client_(nullptr), topic_(OutTopic::Telemetry), dev_(kNoDevice), stamp_(false), failed_(failed), w_(buf_, N) {
    w_.beginObject();
  }
```

The copy constructor also copies `failed_(o.failed_)` (place it after `ts_(o.ts_)`). `send()` starts with
`if (!client_) return failed_;`. Declare the member as `Result failed_ = Result::Ok;` after `uint64_t ts_ = 0;`.

- [ ] **Step 5: Create `src/vl/api/Gateway.h`**

```cpp
#pragma once

#include "vl/Config.h"

#if VL_ENABLE_GATEWAY

#include <stddef.h>
#include <stdint.h>

#include "vl/Client.h"
#include "vl/IApiModule.h"
#include "vl/Payload.h"
#include "vl/Result.h"

namespace vl {

class GatewayBase;

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
  // Queues a disconnect event if name is connected. The table slot is kept,
  // so records already queued for it are still delivered. Thread-safe.
  Result disconnect(const char* name);

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
  GatewayBase(Device* devices, size_t cap) : devices_(devices), cap_(cap) {}

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

  Device* devices_;
  size_t cap_;
  size_t count_ = 0;
};

// Gateway with room for N sub-devices and Methods server-side RPC methods.
template <size_t N, size_t Methods = 4>
class Gateway : public GatewayBase {
  static_assert(N > 0 && N < kNoDevice, "sub-device index must fit RecordMeta::dev");

 public:
  Gateway() : GatewayBase(devices_, N) {}

 private:
  Device devices_[N];
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
```

`Methods` is unused until Task 3 adds the method table. `SubDevice` reads `gw_->client_`, the protected member that
`ApiModule` declares. That works because `SubDevice` is a friend of `GatewayBase` and accesses the member through a
`GatewayBase*`.

- [ ] **Step 6: Create `src/vl/api/Gateway.cpp`**

```cpp
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
  (void)topic;
  (void)payload;
  return false;  // Tasks 3 and 4
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
```

- [ ] **Step 7: Run, expect PASS.** `pio test -e native -f test_gateway`: 7 tests pass. Then `pio test -e native`: every
  suite passes. That covers the `PayloadBuilder` change.

- [ ] **Step 8: Portability.** In `test/portability/portability.ino`, add after the existing module globals:

```cpp
#if VL_ENABLE_GATEWAY
vl::Gateway<2> gw;
#endif
```

and in `setup()`, after the existing `client.use(...)` line:

```cpp
#if VL_ENABLE_GATEWAY
  client.use(gw);
  gw.device("Meter-1", "meter").telemetry().add("kwh", 1.5f).send();
#endif
```

Also add `#include "vl/api/Gateway.h"` to the sketch's includes; `ViraLink.h` gets it in Task 5. Run
`scripts/check_portability.sh`. Expected: exit 0. The gateway is built on `nucleo_f401re` and `nodemcuv2` and skipped
on AVR (Lite).

---

### Task 3: Sub-device RPC

**Files:**
- Modify: `src/vl/api/Rpc.h`, `src/vl/api/Rpc.cpp`, `src/vl/api/Gateway.h`, `src/vl/api/Gateway.cpp`,
  `test/test_gateway/test_main.cpp`

**Interfaces:**
- Consumes: `MethodTable<Fn>`, `RpcReply::unknownMethod`, `RpcReply::finish` (Task 1); `codec::gwRpcReply`.
- Produces:

```cpp
typedef void (*GwRpcHandler)(const char* device, const RpcRequest& req, RpcReply& reply, void* ctx);
Result GatewayBase::on(const char* method, GwRpcHandler fn, void* ctx = nullptr);  // call before begin()
// RpcReply, private: gateway mode; publishes {"device":…,"id":…,"data":<reply>} on v1/gateway/rpc
RpcReply(Client* c, const char* device, uint32_t id, char* buf, size_t cap);
```

- [ ] **Step 1: Failing tests.** Add the helpers inside the anonymous namespace (after `expect`), then the tests, and
  register them in `main`. Add the `rpc()` member to `GwRig`:

```cpp
  // Delivers a gateway RPC and returns the single reply it produced.
  std::string rpc(const char* body) {
    t.message("v1/gateway/rpc", body);
    std::vector<Published> p = flush();
    TEST_ASSERT_EQUAL_UINT(1, p.size());
    TEST_ASSERT_EQUAL_STRING("v1/gateway/rpc", p[0].topic.c_str());
    return p[0].payload;
  }
```

```cpp
void onSetValve(const char* device, const RpcRequest& req, RpcReply& reply, void*) {
  reply.json().beginObject().key("device").value(device).key("open").value(req.params()["open"].asBool()).endObject();
  reply.send();
}
void silent(const char*, const RpcRequest&, RpcReply&, void*) {}
```

```cpp
static void test_rpc_dispatch_and_reply() {
  GwRig r;
  TEST_ASSERT_EQUAL(Result::Ok, r.gw.on("setValve", onSetValve));
  r.connect();
  TEST_ASSERT_EQUAL_STRING(
      "{\"device\":\"Valve-3\",\"id\":7,\"data\":{\"device\":\"Valve-3\",\"open\":true}}",
      r.rpc("{\"device\":\"Valve-3\",\"data\":{\"id\":7,\"method\":\"setValve\",\"params\":{\"open\":true}}}").c_str());
}

static void test_rpc_automatic_replies() {
  GwRig r;
  r.gw.on("silent", silent);
  r.connect();
  TEST_ASSERT_EQUAL_STRING("{\"device\":\"V\",\"id\":1,\"data\":{\"error\":\"unknown method: nope\"}}",
                           r.rpc("{\"device\":\"V\",\"data\":{\"id\":1,\"method\":\"nope\"}}").c_str());
  TEST_ASSERT_EQUAL_STRING("{\"device\":\"V\",\"id\":2,\"data\":{\"error\":\"no reply\"}}",
                           r.rpc("{\"device\":\"V\",\"data\":{\"id\":2,\"method\":\"silent\"}}").c_str());
  TEST_ASSERT_EQUAL_STRING("{\"device\":\"V\",\"id\":3,\"data\":{\"error\":\"bad request\"}}",
                           r.rpc("{\"device\":\"V\",\"data\":{\"id\":3}}").c_str());
  // Without a device or an id there is nobody to answer.
  r.t.message("v1/gateway/rpc", "{\"data\":{\"id\":4,\"method\":\"silent\"}}");
  r.t.message("v1/gateway/rpc", "{\"device\":\"V\",\"data\":{\"method\":\"silent\"}}");
  r.t.message("v1/gateway/rpc", "not json");
  TEST_ASSERT_EQUAL_UINT(0, r.flush().size());
}

static void test_rpc_reply_goes_first() {
  GwRig r;
  r.gw.on("setValve", onSetValve);
  r.connect();
  r.gw.device("A").telemetry().add("n", 1).send();
  r.t.message("v1/gateway/rpc", "{\"device\":\"A\",\"data\":{\"id\":1,\"method\":\"setValve\",\"params\":{\"open\":false}}}");
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(3, p.size());
  expect(p[0], "v1/gateway/rpc", "{\"device\":\"A\",\"id\":1,\"data\":{\"device\":\"A\",\"open\":false}}");
  TEST_ASSERT_EQUAL_STRING("v1/gateway/connect", p[1].topic.c_str());
}

static void test_rpc_registry_validation() {
  Gateway<1, 1> g;
  TEST_ASSERT_EQUAL(Result::Rejected, g.on(nullptr, silent));
  TEST_ASSERT_EQUAL(Result::Rejected, g.on("a", nullptr));
  std::string longName(VL_RPC_METHOD_SIZE, 'm');
  TEST_ASSERT_EQUAL(Result::Rejected, g.on(longName.c_str(), silent));
  TEST_ASSERT_EQUAL(Result::Ok, g.on("a", silent));
  TEST_ASSERT_EQUAL(Result::Ok, g.on("a", onSetValve));  // replaces
  TEST_ASSERT_EQUAL(Result::NoMemory, g.on("b", silent));
}
```

- [ ] **Step 2: Run, expect FAIL.** `pio test -e native -f test_gateway`. Expected: compile error, no member named `on`
  in `Gateway<2>`.

- [ ] **Step 3: RpcReply gateway mode.** In `src/vl/api/Rpc.h`, add a `const char* device_` member, declared between
  `client_` and `id_` so the initializer order matches (`-Wreorder` is an error), and the constructor, with the
  existing one delegating:

```cpp
  RpcReply(Client* c, uint32_t id, char* buf, size_t cap) : RpcReply(c, nullptr, id, buf, cap) {}
  // device != null: a sub-device RPC answered on v1/gateway/rpc. device must
  // outlive the reply.
  RpcReply(Client* c, const char* device, uint32_t id, char* buf, size_t cap)
      : client_(c), device_(device), id_(id), buf_(buf), cap_(cap), w_(buf, cap) {}
```

In `src/vl/api/Rpc.cpp`, `publish` wraps the body for the gateway:

```cpp
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
```

- [ ] **Step 4: Gateway registry and dispatch.** In `src/vl/api/Gateway.h`, include `vl/api/MethodTable.h` and
  `vl/api/Rpc.h`, then add:

```cpp
// Server-side RPC addressed to a sub-device. device is valid during the call.
typedef void (*GwRpcHandler)(const char* device, const RpcRequest& req, RpcReply& reply, void* ctx);
```

Add to `GatewayBase` (public):

```cpp
  // Registers or replaces the handler for a sub-device RPC method; the name
  // is copied. Every request gets a reply, as with Rpc. Call before begin().
  Result on(const char* method, GwRpcHandler fn, void* ctx = nullptr) { return methods_.add(method, fn, ctx); }
```

The protected section gets `typedef MethodTable<GwRpcHandler>::Entry Method;`, and the constructor becomes
`GatewayBase(Device* devices, size_t cap, Method* methods, size_t methodCap) : devices_(devices), cap_(cap),
methods_(methods, methodCap) {}`. The private section gets `void serveRpc(Bytes payload);`,
`MethodTable<GwRpcHandler> methods_;` and `char reply_[VL_RPC_REPLY_SIZE];`. `Gateway<N, Methods>` becomes:

```cpp
  Gateway() : GatewayBase(devices_, N, methods_, Methods) {}

 private:
  Device devices_[N];
  Method methods_[Methods];
```

In `src/vl/api/Gateway.cpp`:

```cpp
bool GatewayBase::handle(const Topic& topic, Bytes payload) {
  switch (topic.kind) {
    case InTopic::GwRpc:
      serveRpc(payload);
      return true;
    default:
      return false;  // Task 4 adds the attribute topics
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
```

- [ ] **Step 5: Run, expect PASS.** `pio test -e native -f test_gateway` (11 tests), then `pio test -e native -f test_rpc`
  (device replies unchanged), then `scripts/check_portability.sh`.

---

### Task 4: Sub-device attribute updates and requests

**Files:**
- Modify: `src/vl/api/Requests.h`, `src/vl/api/Attributes.h`, `src/vl/api/Attributes.cpp`, `src/vl/api/Gateway.h`,
  `src/vl/api/Gateway.cpp`, `test/test_gateway/test_main.cpp`

**Interfaces:**
- Consumes: `RequestSlots`, `Client::resolvePending`, `codec::gwAttributeRequest`.
- Produces:

```cpp
// Attributes.h
typedef void (*AttrVisitor)(const char* key, JsonValue value, void* ctx);
bool forEachAttribute(JsonValue attrs, AttrVisitor fn, void* ctx);
// Requests.h, RequestSlots<Cb, N>
uint32_t open(Client& c, const IApiModule* owner, uint32_t timeoutMs, Correlator::Callback done, Cb cb, void* ctx,
              void*& handle);
Result submit(Client& c, uint32_t id, void* handle, const RecordMeta& meta, const char* payload, size_t len);
void abort(Client& c, uint32_t id, void* handle);
// Gateway.h
typedef void (*GwAttrUpdateCb)(const char* device, const char* key, JsonValue value, void* ctx);
typedef void (*GwAttrResponseCb)(Result r, JsonValue response, void* ctx);
void GatewayBase::onAttributeUpdate(GwAttrUpdateCb cb, void* ctx = nullptr);
Result SubDevice::requestAttributes(bool client, const char* keys, GwAttrResponseCb cb, void* ctx,
                                    uint32_t timeoutMs = VL_ATTR_TIMEOUT_MS) const;
```

- [ ] **Step 1: Failing tests.** Add the helpers inside the anonymous namespace, the tests after it, and register them
  in `main`:

```cpp
std::vector<std::string> updates;
void onGwUpdate(const char* device, const char* key, JsonValue v, void*) {
  updates.push_back(std::string(device) + "/" + key + "=" +
                    (v.valid() ? std::string(v.raw(), v.rawSize()) : std::string("<deleted>")));
}

struct GwResponse {
  int n = 0;
  Result r = Result::Ok;
  std::string device, fw, value;
} gwResp;
void onGwResponse(Result r, JsonValue response, void*) {
  ++gwResp.n;
  gwResp.r = r;
  char buf[16] = "";
  response["device"].copyString(buf, sizeof(buf));
  gwResp.device = buf;
  buf[0] = '\0';
  response["values"]["fw"].copyString(buf, sizeof(buf));
  gwResp.fw = buf;
  buf[0] = '\0';
  response["value"].copyString(buf, sizeof(buf));
  gwResp.value = buf;
}

uint32_t requestId(const Published& p) {
  TEST_ASSERT_EQUAL_STRING("v1/gateway/attributes/request", p.topic.c_str());
  return static_cast<uint32_t>(JsonValue::parse(p.payload.data(), p.payload.size())["id"].asInt());
}
```

```cpp
static void test_attribute_updates_per_device() {
  GwRig r;
  updates.clear();
  r.gw.onAttributeUpdate(onGwUpdate);
  r.connect();
  r.t.message("v1/gateway/attributes", "{\"device\":\"A\",\"data\":{\"mode\":\"eco\",\"deleted\":[\"old\"]}}");
  TEST_ASSERT_EQUAL_UINT(2, updates.size());
  TEST_ASSERT_EQUAL_STRING("A/mode=\"eco\"", updates[0].c_str());
  TEST_ASSERT_EQUAL_STRING("A/old=<deleted>", updates[1].c_str());
  r.t.message("v1/gateway/attributes", "{\"device\":\"A\"}");
  r.t.message("v1/gateway/attributes", "{\"data\":{\"k\":1}}");
  r.t.message("v1/gateway/attributes", "[]");
  TEST_ASSERT_EQUAL_UINT(2, updates.size());
}

static void test_attribute_request_and_response() {
  GwRig r;
  r.connect();
  SubDevice a = r.gw.device("A");
  r.flush();
  gwResp = GwResponse();
  TEST_ASSERT_EQUAL(Result::Ok, a.requestAttributes(true, "fw,model", onGwResponse, nullptr));
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(1, p.size());
  unsigned long id = requestId(p[0]);
  char text[128];
  snprintf(text, sizeof(text), "{\"id\":%lu,\"device\":\"A\",\"client\":true,\"keys\":[\"fw\",\"model\"]}", id);
  TEST_ASSERT_EQUAL_STRING(text, p[0].payload.c_str());
  snprintf(text, sizeof(text), "{\"id\":%lu,\"device\":\"A\",\"values\":{\"fw\":\"1.0\",\"model\":\"X\"}}", id);
  r.t.message("v1/gateway/attributes/response", text);
  TEST_ASSERT_EQUAL(1, gwResp.n);
  TEST_ASSERT_EQUAL(Result::Ok, gwResp.r);
  TEST_ASSERT_EQUAL_STRING("A", gwResp.device.c_str());
  TEST_ASSERT_EQUAL_STRING("1.0", gwResp.fw.c_str());

  // One attribute found: the server answers with "value".
  TEST_ASSERT_EQUAL(Result::Ok, a.requestAttributes(false, "mode", onGwResponse, nullptr));
  p = r.flush();
  id = requestId(p[0]);
  snprintf(text, sizeof(text), "{\"id\":%lu,\"device\":\"A\",\"value\":\"eco\"}", id);
  r.t.message("v1/gateway/attributes/response", text);
  TEST_ASSERT_EQUAL(2, gwResp.n);
  TEST_ASSERT_EQUAL_STRING("eco", gwResp.value.c_str());

  r.t.message("v1/gateway/attributes/response", "{\"id\":999,\"device\":\"A\",\"value\":1}");  // unknown id
  r.t.message("v1/gateway/attributes/response", "not json");
  TEST_ASSERT_EQUAL(2, gwResp.n);
}

static void test_attribute_request_timeout_and_limits() {
  GwRig r;
  r.connect();
  SubDevice a = r.gw.device("A");
  TEST_ASSERT_EQUAL(Result::Rejected, a.requestAttributes(true, nullptr, onGwResponse, nullptr));
  TEST_ASSERT_EQUAL(Result::Rejected, a.requestAttributes(true, "", onGwResponse, nullptr));
  TEST_ASSERT_EQUAL(Result::Rejected, a.requestAttributes(true, "fw", nullptr, nullptr));
  TEST_ASSERT_EQUAL(Result::Rejected, r.gw.find("zz").requestAttributes(true, "fw", onGwResponse, nullptr));
  std::string many = "key";
  for (int i = 0; i < 200; ++i) many += ",key";
  // Too large: the request is opened, then abandoned, so its slot comes back.
  TEST_ASSERT_EQUAL(Result::TooLarge, a.requestAttributes(true, many.c_str(), onGwResponse, nullptr));
  gwResp = GwResponse();
  for (int i = 0; i < VL_GW_MAX_REQUESTS; ++i) {
    TEST_ASSERT_EQUAL(Result::Ok, a.requestAttributes(true, "fw", onGwResponse, nullptr));
  }
  TEST_ASSERT_EQUAL(Result::NoMemory, a.requestAttributes(true, "fw", onGwResponse, nullptr));
  r.clock.advance(VL_ATTR_TIMEOUT_MS);
  r.c.loop();
  TEST_ASSERT_EQUAL(VL_GW_MAX_REQUESTS, gwResp.n);
  TEST_ASSERT_EQUAL(Result::Timeout, gwResp.r);
}
```

- [ ] **Step 2: Run, expect FAIL.** `pio test -e native -f test_gateway`. Expected: compile error, no member named
  `onAttributeUpdate`.

- [ ] **Step 3: Extract `forEachAttribute`.** In `src/vl/api/Attributes.h`, before `SharedAttributesBase`:

```cpp
typedef void (*AttrVisitor)(const char* key, JsonValue value, void* ctx);
// Calls fn for every member of an attribute object and, with an Invalid
// value, for every name in its "deleted" array. Keys longer than
// VL_ATTR_KEYS_SIZE - 1 are skipped. False if attrs is not an object.
bool forEachAttribute(JsonValue attrs, AttrVisitor fn, void* ctx);
```

In `src/vl/api/Attributes.cpp`, move the loop out of `SharedAttributesBase::handle`:

```cpp
bool forEachAttribute(JsonValue attrs, AttrVisitor fn, void* ctx) {
  if (attrs.type() != JsonValue::Type::Object) return false;
  char key[VL_ATTR_KEYS_SIZE];
  JsonValue k, v;
  for (size_t i = 0; attrs.member(i, k, v); ++i) {
    if (!k.copyString(key, sizeof(key))) continue;
    if (strcmp(key, "deleted") == 0 && v.type() == JsonValue::Type::Array) {
      for (size_t j = 0; j < v.size(); ++j) {
        if (v.at(j).copyString(key, sizeof(key))) fn(key, JsonValue(), ctx);
      }
      continue;
    }
    fn(key, v, ctx);
  }
  return true;
}
```

`SharedAttributesBase::handle` then ends with:

```cpp
  if (!forEachAttribute(JsonValue::parse(payload), visitUpdate, this)) VL_LOGW("attributes: update is not a JSON object");
  return true;
```

`visitUpdate` is a new private static on `SharedAttributesBase`:
`static void visitUpdate(const char* key, JsonValue v, void* ctx) { static_cast<SharedAttributesBase*>(ctx)->dispatch(key, v); }`.
Run `pio test -e native -f test_attributes`. Expected: PASS, unchanged.

- [ ] **Step 4: Split `RequestSlots::start`.** In `src/vl/api/Requests.h`, add the three methods and rebuild `start` on
  top of them:

```cpp
  // For payloads that embed the request id: claims a slot and a correlator
  // entry and returns the id, or 0 (NoMemory) when either is full. Follow
  // with submit() or abort().
  uint32_t open(Client& c, const IApiModule* owner, uint32_t timeoutMs, Correlator::Callback done, Cb cb, void* ctx,
                void*& handle) {
    Slot* slot = nullptr;
    {
      LockGuard g(c.lock());
      for (size_t i = 0; i < N && !slot; ++i) {
        if (slots_[i].used) continue;
        slot = &slots_[i];
        slot->client = &c;
        slot->cb = cb;
        slot->ctx = ctx;
        slot->used = true;
      }
    }
    if (!slot) return 0;
    uint32_t id = c.addPending(timeoutMs, done, slot, owner);
    if (id == 0) {
      release(slot);
      return 0;
    }
    handle = slot;
    return id;
  }

  // Queues an opened request; on failure it is dropped without firing.
  Result submit(Client& c, uint32_t id, void* handle, const RecordMeta& meta, const char* payload, size_t len) {
    Result r = c.enqueue(meta, reinterpret_cast<const uint8_t*>(payload), len);
    if (!ok(r)) abort(c, id, handle);
    return r;
  }

  // Drops an opened request without firing it.
  void abort(Client& c, uint32_t id, void* handle) {
    // If the cancel loses a race with the timeout, done frees the slot.
    if (c.cancelPending(id)) release(static_cast<Slot*>(handle));
  }

  Result start(Client& c, const IApiModule* owner, RecordMeta meta, const char* payload, size_t len,
               uint32_t timeoutMs, Correlator::Callback done, Cb cb, void* ctx) {
    void* handle = nullptr;
    uint32_t id = open(c, owner, timeoutMs, done, cb, ctx, handle);
    if (id == 0) return Result::NoMemory;
    meta.arg = id;
    return submit(c, id, handle, meta, payload, len);
  }
```

Run `pio test -e native -f test_rpc` and `-f test_attributes`. Expected: PASS, unchanged.

- [ ] **Step 5: Gateway attributes.** In `src/vl/api/Gateway.h`, include `vl/api/Attributes.h` and `vl/api/Requests.h`,
  and add:

```cpp
// A shared attribute of a sub-device changed; value is Invalid when it was
// deleted. device and value are valid during the call.
typedef void (*GwAttrUpdateCb)(const char* device, const char* key, JsonValue value, void* ctx);
// Outcome of a sub-device attribute request. When r is Ok, response is the
// server's object: {"id":…,"device":…,"values":{…}}, or "value":… instead of
// "values" when exactly one attribute was found. Invalid otherwise (Timeout,
// NotReady after end(), Rejected for a malformed reply).
typedef void (*GwAttrResponseCb)(Result r, JsonValue response, void* ctx);
```

`SubDevice` gets (public):

```cpp
  // Requests client (client = true) or shared attributes of this sub-device.
  // keys is comma-separated. cb fires once with the response or Timeout.
  Result requestAttributes(bool client, const char* keys, GwAttrResponseCb cb, void* ctx,
                           uint32_t timeoutMs = VL_ATTR_TIMEOUT_MS) const;
```

`GatewayBase` gets a public `void onAttributeUpdate(GwAttrUpdateCb cb, void* ctx = nullptr)` (sets
`attrCb_`/`attrCtx_`; call before `begin()`). Its private section gets:

```cpp
  Result requestAt(size_t index, bool client, const char* keys, GwAttrResponseCb cb, void* ctx, uint32_t timeoutMs);
  void dispatchUpdate(Bytes payload);
  void resolveResponse(Bytes payload);
  static void visitUpdate(const char* key, JsonValue value, void* ctx);
  static void onResponse(Result r, const uint8_t* p, size_t n, void* ctx);

  GwAttrUpdateCb attrCb_ = nullptr;
  void* attrCtx_ = nullptr;
  RequestSlots<GwAttrResponseCb, VL_GW_MAX_REQUESTS> requests_;
```

In `src/vl/api/Gateway.cpp`:

```cpp
Result SubDevice::requestAttributes(bool client, const char* keys, GwAttrResponseCb cb, void* ctx,
                                    uint32_t timeoutMs) const {
  if (!keys || !keys[0] || !cb) return Result::Rejected;
  Result r = usable();
  return r == Result::Ok ? gw_->requestAt(index_, client, keys, cb, ctx, timeoutMs) : r;
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
```

The `handle` switch adds `case InTopic::GwAttrUpdate: dispatchUpdate(payload); return true;` and
`case InTopic::GwAttrResponse: resolveResponse(payload); return true;`.

- [ ] **Step 6: Run, expect PASS.** `pio test -e native` (every suite; `test_gateway` has 14 tests), then
  `scripts/check_portability.sh`.

---

### Task 5: Public header, example, docs, gate

**Files:**
- Modify: `src/ViraLink.h`, `README.md`, `library.json`, `test/portability/portability.ino`,
  `docs/superpowers/plans/2026-10-25-roadmap.md`
- Create: `examples/gateway/gateway.ino`

- [ ] **Step 1: Public header.** Add `#include "vl/api/Gateway.h"` to `src/ViraLink.h` after `vl/api/Attributes.h`.
  Remove the now redundant direct include from `test/portability/portability.ino`.

- [ ] **Step 2: Example** `examples/gateway/gateway.ino`:

```cpp
// A gateway reporting for two energy meters on one MQTT session.
//
// - Each meter is a sub-device: connected on first use, and again after
//   every reconnect, with telemetry every 10 s.
// - RPC "setValve" {"open":true} sent to a meter's device page is answered
//   with {"open":true}.
// - Shared-attribute updates for any meter are printed.

#include <WiFi.h>
#include <ViraLink.h>
#include <time.h>

static const char* WIFI_SSID = "your-ssid";
static const char* WIFI_PASS = "your-password";
static const char* VL_HOST = "console.viralink.ir";
static const char* VL_TOKEN = "YOUR_GATEWAY_TOKEN";  // a device with "Is gateway" set

static const char* METERS[] = {"Meter-1", "Meter-2"};
static const size_t METER_COUNT = sizeof(METERS) / sizeof(METERS[0]);

vl::EspMqtt mqtt;
vl::RingOutbox<16 * 1024> outbox;
vl::Client client(mqtt, outbox);
vl::Gateway<4> gw;  // up to 4 sub-devices

static float kwh[METER_COUNT];

static void setValve(const char* device, const vl::RpcRequest& req, vl::RpcReply& reply, void* /*ctx*/) {
  vl::JsonValue open = req.params()["open"];
  if (open.type() != vl::JsonValue::Type::Bool) {
    reply.error("params.open must be true or false");
    return;
  }
  Serial.printf("%s: valve %s\n", device, open.asBool() ? "open" : "closed");
  reply.json().beginObject().key("open").value(open.asBool()).endObject();
  reply.send();
}

static void onAttribute(const char* device, const char* key, vl::JsonValue value, void* /*ctx*/) {
  if (!value.valid()) {
    Serial.printf("%s: %s deleted\n", device, key);
    return;
  }
  Serial.printf("%s: %s = %.*s\n", device, key, static_cast<int>(value.rawSize()), value.raw());
}

static void readMeters(void* /*ctx*/) {
  for (size_t i = 0; i < METER_COUNT; ++i) {
    kwh[i] += 0.01f * (i + 1);  // stands in for a Modbus read
    vl::Result r = gw.device(METERS[i], "energy-meter").telemetry().add("kwh", kwh[i]).send();
    if (!vl::ok(r)) Serial.printf("%s: telemetry not queued: %s\n", METERS[i], vl::toString(r));
  }
}

void setup() {
  Serial.begin(115200);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, "pool.ntp.org");

  gw.on("setValve", setValve);
  gw.onAttributeUpdate(onAttribute);
  client.use(gw);

  client.onState([](vl::State s, void*) { Serial.printf("state: %s\n", vl::toString(s)); }, nullptr);
  client.every(10000, readMeters, nullptr);

  vl::Config cfg;
  cfg.host = VL_HOST;
  cfg.auth = vl::AccessToken(VL_TOKEN);
  client.begin(cfg);
}

void loop() {
  client.loop();
  delay(5);
}
```

- [ ] **Step 3: Docs.** In `library.json`, add the example after `rpc_and_attributes`, keeping the inline-array style:

```json
    {
      "name": "gateway",
      "base": "examples/gateway",
      "files": ["gateway.ino"]
    }
```

In `README.md`: drop "gateway" from the status line's "Still to come" list, add `examples/gateway` to the examples
line, and add this section before `### Claiming`:

````markdown
### Gateway

One MQTT session can speak for several sub-devices (the device must have "Is gateway" set on the server):

```cpp
vl::Gateway<16> gw;                               // up to 16 sub-devices
client.use(gw);

gw.on("setValve", [](const char* device, const vl::RpcRequest& req, vl::RpcReply& reply, void*) {
  reply.json().beginObject().key("open").value(req.params()["open"].asBool()).endObject();
  reply.send();
});
gw.onAttributeUpdate([](const char* device, const char* key, vl::JsonValue value, void*) { /* ... */ });

vl::SubDevice meter = gw.device("Meter-12", "energy-meter");  // connects on first use
meter.telemetry().add("kwh", 3.5).send();
meter.requestAttributes(false, "mode", onMode, nullptr);       // response["values"] or ["value"]
meter.disconnect();
```

A sub-device's connect event always precedes its data, and every connected
sub-device is connected again after a reconnect. Sub-device names stay in the
table for the life of the gateway, so `N` is the number of distinct
sub-devices per boot. Claiming works per sub-device: `meter.claim("secret", 60000)`.
````

- [ ] **Step 4: Gate.** Run `pio test -e native`: every suite passes. Run `scripts/check_portability.sh`: exit 0. Run
  `scripts/build_examples.sh`: exit 0, with `gateway.ino` built for `esp32dev` with `-Wall -Werror`.

- [ ] **Step 5: Roadmap.** In `docs/superpowers/plans/2026-10-25-roadmap.md`, tick the four Phase 3 items and change the
  heading to `## Phase 3: Gateway (done; plan: \`2026-10-25-phase3-gateway.md\`)`.
