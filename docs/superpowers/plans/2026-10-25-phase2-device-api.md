# Phase 2: Device API Modules Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Server- and client-side RPC, attribute requests, shared-attribute updates and binding, client-attribute reporting and device claiming, all heap-free and host-tested.

**Architecture:** Each API is an `IApiModule` in `src/vl/api/`, attached with `client.use()`. Incoming JSON is read by a new zero-copy `JsonValue` (no ArduinoJson, so it builds on AVR and the native host with no dependency). Outgoing payloads go through `core/Codec` and the outbox; request/response pairs use the existing `Correlator` through `Client::addPending`.

**Tech Stack:** C++11 (`-std=gnu++11`), PlatformIO `native` + Unity, ASan/UBSan, `-Werror`.

## Global Constraints

- C++11 in `src/`; no heap, `std::string`, `std::function`, exceptions.
- Capacities are template parameters or `VL_*` macros in `Config.h`; full tables return `Result::NoMemory`.
- Callbacks are function pointers plus `void* ctx`, fired on the client task with no lock held.
- Every payload shape lives in `core/Codec` with a golden-string test.
- Strings passed in by the caller are copied if kept beyond the call.
- RPC replies use `RecordClass::Priority`; requests and claims use `RecordClass::Control`.
- Every server RPC gets a reply, including unknown methods and unanswered handlers.
- Gate: `pio test -e native`, `scripts/check_portability.sh` and `scripts/build_examples.sh` pass.
- Portable to every target class: 8-bit AVR (16-bit `int`, 32-bit `double`, no libstdc++, no float printf),
  Cortex-M (newlib-nano: no float printf by default), ESP8266, ESP32 (Xtensa and RISC-V). So in `src/`: no C++
  standard library headers outside `platform/`, no `printf` of floats or `%ll`/`%z`, no shifts wider than `int`
  without a cast, sizes from `VL_*` macros with smaller Lite defaults, no identifiers Arduino.h defines as macros
  (`min`, `max`, `abs`, `round`, `bit`, `B0`…`B11111111`, `DEC`, `HEX`, …).

## Decisions

- **JSON input:** in-house `JsonValue` instead of ArduinoJson v7. ArduinoJson allocates from a pool and is an extra dependency; the reader here validates once, then walks the payload lazily with no copies. Nesting is capped at `VL_JSON_MAX_DEPTH` to bound stack use.
- **Attribute requests** live on `SharedAttributes<N>`, which owns the response topic.
- **Deletion notice** resets a bound variable to zero / false / empty string.
- **Client attributes** are reported once on the first tick, then only on change.

## File Structure

| File | Responsibility |
| --- | --- |
| `src/vl/core/JsonReader.h/.cpp` | `JsonValue`: validate, look up, iterate, convert |
| `src/vl/core/Codec.h/.cpp` | add `rpcError`, `rpcRequest` |
| `src/vl/Client.h/.cpp` | add `cancelPending`, `lock()`, `claim()` |
| `src/vl/api/Rpc.h/.cpp` | `Rpc<N>`, `RpcRequest`, `RpcReply`, client `call()` |
| `src/vl/api/Attributes.h/.cpp` | `SharedAttributes<N>`, `ClientAttributes<N>` |
| `src/vl/Config.h` | new `VL_*` sizes |
| `src/ViraLink.h` | include the new modules |
| `test/support/ClientRig.h` | shared client + fake transport rig |
| `test/test_json/`, `test/test_rpc/`, `test/test_attributes/` | new suites |
| `examples/rpc_and_attributes/` | Arduino example |

New `Config.h` values: `VL_JSON_MAX_DEPTH 16`, `VL_RPC_METHOD_SIZE 32`,
`VL_RPC_REPLY_SIZE 256` (Lite 96), `VL_RPC_MAX_CALLS 4` (Lite 1),
`VL_RPC_TIMEOUT_MS 10000`, `VL_ATTR_KEYS_SIZE 64`, `VL_ATTR_MAX_REQUESTS 4`
(Lite 1), `VL_ATTR_TIMEOUT_MS 10000`.

---

### Task 1: JSON reader

**Files:** Create `src/vl/core/JsonReader.h`, `src/vl/core/JsonReader.cpp`, `test/test_json/test_main.cpp`; modify `src/vl/Config.h`.

**Produces:**

```cpp
class JsonValue {
 public:
  enum class Type : uint8_t { Invalid, Null, Bool, Number, String, Array, Object };
  JsonValue();
  static JsonValue parse(const char* json, size_t n);  // Invalid unless the whole span is one value
  static JsonValue parse(Bytes b);
  Type type() const;
  bool valid() const;
  JsonValue operator[](const char* key) const;   // Invalid if missing or not an object
  JsonValue at(size_t i) const;                  // Invalid if out of range or not an array
  size_t size() const;                           // members or elements; 0 otherwise
  bool member(size_t i, JsonValue& key, JsonValue& value) const;
  bool asBool(bool def = false) const;
  long long asInt(long long def = 0) const;      // numbers only, truncated toward zero
  double asDouble(double def = 0) const;
  bool copyString(char* out, size_t cap) const;  // unescaped, NUL-terminated; false if not a string or too long
  bool equals(const char* s) const;              // string value equals s after unescaping
  const char* raw() const;
  size_t rawSize() const;
};
```

- [ ] **Step 1: Write the failing test** (`test/test_json/test_main.cpp`)

```cpp
// JsonValue: validation, lookup, iteration and conversions over spans that
// are not NUL-terminated.
#include <math.h>
#include <string.h>
#include <unity.h>

#include <string>

#include "vl/core/JsonReader.h"

using namespace vl;

void setUp() {}
void tearDown() {}

static JsonValue P(const char* s) { return JsonValue::parse(s, strlen(s)); }

static void test_scalars() {
  TEST_ASSERT_EQUAL(JsonValue::Type::Null, P("null").type());
  TEST_ASSERT_TRUE(P("true").asBool());
  TEST_ASSERT_FALSE(P("false").asBool(true));
  TEST_ASSERT_EQUAL_INT(-42, static_cast<int>(P(" -42 ").asInt()));
  TEST_ASSERT_TRUE(fabs(P("2.15e1").asDouble() - 21.5) < 1e-9);
  TEST_ASSERT_EQUAL_INT(7, static_cast<int>(P("7.9").asInt()));
  TEST_ASSERT_EQUAL_INT(5, static_cast<int>(P("\"x\"").asInt(5)));
  TEST_ASSERT_TRUE(P("1").asBool(true));  // wrong type: default
}

static void test_rejects_invalid() {
  const char* bad[] = {"",       "{",       "{\"a\":}", "[1,]", "{\"a\" 1}",     "tru",      "01",   "1.",
                       "\"open", "{} x",    "{\"a\":1,}", "-",  "\"bad\\q\"",    "[1 2]",    "{1:2}", "\"a\x01\""};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) TEST_ASSERT_FALSE_MESSAGE(P(bad[i]).valid(), bad[i]);
}

static void test_depth_limit() {
  std::string ok = std::string(VL_JSON_MAX_DEPTH, '[') + std::string(VL_JSON_MAX_DEPTH, ']');
  std::string deep = std::string(VL_JSON_MAX_DEPTH + 1, '[') + std::string(VL_JSON_MAX_DEPTH + 1, ']');
  TEST_ASSERT_TRUE(P(ok.c_str()).valid());
  TEST_ASSERT_FALSE(P(deep.c_str()).valid());
}

static void test_object_lookup_and_iteration() {
  JsonValue v = P("{\"method\":\"setRelay\",\"params\":{\"on\":true,\"ch\":[1,2,3]},\"x\":null}");
  TEST_ASSERT_EQUAL(JsonValue::Type::Object, v.type());
  TEST_ASSERT_EQUAL_UINT(3, v.size());
  TEST_ASSERT_TRUE(v["method"].equals("setRelay"));
  TEST_ASSERT_TRUE(v["params"]["on"].asBool());
  TEST_ASSERT_EQUAL_UINT(3, v["params"]["ch"].size());
  TEST_ASSERT_EQUAL_INT(3, static_cast<int>(v["params"]["ch"].at(2).asInt()));
  TEST_ASSERT_FALSE(v["params"]["ch"].at(3).valid());
  TEST_ASSERT_FALSE(v["missing"].valid());
  TEST_ASSERT_FALSE(v["missing"]["deeper"].valid());
  TEST_ASSERT_EQUAL(JsonValue::Type::Null, v["x"].type());
  JsonValue k, val;
  TEST_ASSERT_TRUE(v.member(1, k, val));
  TEST_ASSERT_TRUE(k.equals("params"));
  TEST_ASSERT_EQUAL(JsonValue::Type::Object, val.type());
  TEST_ASSERT_FALSE(v.member(3, k, val));
  JsonValue ch = v["params"]["ch"];
  TEST_ASSERT_EQUAL_UINT(7, ch.rawSize());
  TEST_ASSERT_EQUAL_MEMORY("[1,2,3]", ch.raw(), 7);
}

static void test_strings_unescape() {
  JsonValue v = P("{\"s\":\"a\\\"b\\\\c\\/d\\n\\u00e9\\ud83d\\ude00\"}");
  char out[32];
  TEST_ASSERT_TRUE(v["s"].copyString(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("a\"b\\c/d\n\xc3\xa9\xf0\x9f\x98\x80", out);
  char small[4];
  TEST_ASSERT_FALSE(v["s"].copyString(small, sizeof(small)));
  TEST_ASSERT_TRUE(P("{\"k\\u0065y\":1}")["key"].valid());
  TEST_ASSERT_FALSE(P("1").copyString(out, sizeof(out)));
  TEST_ASSERT_TRUE(P("\"ab\"").equals("ab"));
  TEST_ASSERT_FALSE(P("\"ab\"").equals("abc"));
  TEST_ASSERT_FALSE(P("\"ab\"").equals("a"));
}

static void test_span_not_nul_terminated() {
  const char buf[] = {'{', '"', 'a', '"', ':', '1', '2', '}', '9'};
  TEST_ASSERT_EQUAL_INT(12, static_cast<int>(JsonValue::parse(buf, 8)["a"].asInt()));
  TEST_ASSERT_EQUAL_INT(12, static_cast<int>(JsonValue::parse("123", 2).asInt()));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_scalars);
  RUN_TEST(test_rejects_invalid);
  RUN_TEST(test_depth_limit);
  RUN_TEST(test_object_lookup_and_iteration);
  RUN_TEST(test_strings_unescape);
  RUN_TEST(test_span_not_nul_terminated);
  return UNITY_END();
}
```

- [ ] **Step 2: Run, expect FAIL** — `pio test -e native -f test_json` fails: `vl/core/JsonReader.h` not found.
- [ ] **Step 3: Implement.** Recursive-descent validator `skipValue(p, end, depth)` returning the end pointer or null; `parse` skips whitespace, validates, requires only whitespace after. Lookups re-walk the span with the same skipper (safe: the span is already valid). Numbers are copied into a 32-byte stack buffer for `strtoll`/`strtod`. `copyString` and `equals` share one decoder that handles `\" \\ \/ \b \f \n \r \t \uXXXX` and surrogate pairs, emitting UTF-8.
- [ ] **Step 4: Run, expect PASS** — `pio test -e native -f test_json`.

### Task 2: Codec RPC payloads

**Files:** Modify `src/vl/core/Codec.h/.cpp`, `test/test_codec/test_main.cpp`.

**Produces:** `void codec::rpcError(JsonWriter& w, const char* message);` → `{"error":…}`; `void codec::rpcRequest(JsonWriter& w, const char* method, const char* paramsJson, size_t paramsLen);` → `{"method":…,"params":P}`, params `{}` when null.

- [ ] **Step 1: Failing test** (add and register in `main`)

```cpp
static void test_rpc_payloads() {
  char buf[96];
  {
    JsonWriter w(buf, sizeof(buf));
    codec::rpcError(w, "unknown method: x");
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"unknown method: x\"}", buf);
  }
  {
    JsonWriter w(buf, sizeof(buf));
    codec::rpcRequest(w, "getTime", "{\"tz\":1}", 8);
    TEST_ASSERT_EQUAL_STRING("{\"method\":\"getTime\",\"params\":{\"tz\":1}}", buf);
  }
  {
    JsonWriter w(buf, sizeof(buf));
    codec::rpcRequest(w, "ping", nullptr, 0);
    TEST_ASSERT_EQUAL_STRING("{\"method\":\"ping\",\"params\":{}}", buf);
  }
}
```

- [ ] **Step 2: Run, expect FAIL** (`rpcError` undeclared).
- [ ] **Step 3: Implement** both functions with `JsonWriter`.
- [ ] **Step 4: Run, expect PASS** — `pio test -e native -f test_codec`.

### Task 2a: Portability gate

**Files:** Create `scripts/check_portability.sh`, `test/portability/portability.ino`.

- [ ] **Step 1:** Script stage 1 compiles every platform-free source (`core/`, `api/`, `Client.cpp`, `Log.cpp`)
  alone with avr-g++ (`-mmcu=atmega2560`), arm-none-eabi-g++ (`-mcpu=cortex-m0plus -mthumb`),
  xtensa-lx106-elf-g++ and riscv32-esp-elf-g++, using `-std=gnu++11 -Os -Wall -Wextra -Werror -fno-exceptions
  -fno-rtti`, from the toolchains PlatformIO caches.
- [ ] **Step 2:** Stage 2 links `test/portability/portability.ino` (a transport that never connects, every
  module attached, every value type sent) through `pio ci` for `megaatmega2560`, `nucleo_f401re`, `nodemcuv2`,
  with `lib_compat_mode=off` because those platforms are not advertised yet. Catches Arduino.h macro clashes,
  platform-layer and link errors, and RAM overflow.
- [ ] **Step 3:** Run it; fix what fails; every later task extends the sketch with its module.

### Task 2b: Float formatting without printf

**Files:** Modify `src/vl/core/JsonWriter.h/.cpp`, `test/test_codec/test_main.cpp`.

**Produces:** `size_t json::formatDouble(double v, int digits, char* out);` (out ≥ 32 bytes, `digits` 1–9,
`%.<digits>g` layout); `JsonWriter::value(double)` uses it with `VL_JSON_DOUBLE_DIGITS` (9 with a 64-bit
`double`, 7 with AVR's 32-bit one).

- [ ] **Step 1: Failing test**

```cpp
static std::string fmt(double v, int digits) {
  char buf[32];
  size_t n = json::formatDouble(v, digits, buf);
  return std::string(buf, n);
}

static void test_format_double() {
  TEST_ASSERT_EQUAL_STRING("21.5", fmt(21.5, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("0.100000001", fmt(0.1f, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("0.1", fmt(0.1f, 7).c_str());
  TEST_ASSERT_EQUAL_STRING("-3.25", fmt(-3.25, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("100", fmt(100.0, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("0", fmt(0.0, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("0", fmt(-0.0, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("123456789", fmt(123456789.0, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("1.23456789e+09", fmt(1234567890.0, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("1.234568e+09", fmt(1234567890.0, 7).c_str());
  TEST_ASSERT_EQUAL_STRING("0.0001", fmt(0.0001, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("1e-05", fmt(0.00001, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("10", fmt(9.9999999999, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("3.40282347e+38", fmt(3.4028234663852886e38, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("3.402823e+38", fmt(3.4028234663852886e38, 7).c_str());
  TEST_ASSERT_EQUAL_STRING("1e-300", fmt(1e-300, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("1.79769313e+308", fmt(1.7976931348623157e308, 9).c_str());
}

// Nine digits bring every float back unchanged, as printf("%.9g") does.
static void test_format_double_round_trips_floats() {
  uint32_t x = 12345;
  for (int i = 0; i < 20000; ++i) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    float f;
    memcpy(&f, &x, sizeof(f));
    if (isnan(f) || isinf(f)) continue;
    std::string s = fmt(f, 9);
    TEST_ASSERT_TRUE_MESSAGE(static_cast<float>(strtod(s.c_str(), nullptr)) == f, s.c_str());
  }
}
```

- [ ] **Step 2: Run, expect FAIL** (`json::formatDouble` undeclared).
- [ ] **Step 3: Implement** with integer digit extraction: find the decimal exponent, scale by exact powers of
  ten (two steps beyond 10^300), round to a `digits`-digit integer, strip trailing zeros, lay out as `%g`.
- [ ] **Step 4: Run, expect PASS**, plus every existing JsonWriter golden; run `scripts/check_portability.sh`.

### Task 3: Client additions and test rig

**Files:** Modify `src/vl/Client.h/.cpp`, `test/test_client/test_main.cpp`; create `test/support/ClientRig.h`.

**Produces:** `bool Client::cancelPending(uint32_t id);` (removes without firing), `ILock& Client::lock();`, `Result Client::claim(const char* secretKey, uint32_t durationMs);` (Control record on `v1/devices/me/claim`). `ClientRig` with `connect()` and `flush()` (two `loop()` calls, returns the publishes, PUBACKs them).

- [ ] **Step 1: Failing tests**

```cpp
static void test_cancel_pending_does_not_fire() {
  Rig r;
  r.connect();
  static int fired = 0;
  fired = 0;
  uint32_t id = r.c.addPending(1000, [](Result, const uint8_t*, size_t, void*) { ++fired; }, nullptr);
  TEST_ASSERT_TRUE(r.c.cancelPending(id));
  TEST_ASSERT_FALSE(r.c.cancelPending(id));
  r.clock.advance(2000);
  r.c.loop();
  TEST_ASSERT_EQUAL(0, fired);
}

static void test_claim() {
  Rig r;
  r.connect();
  TEST_ASSERT_EQUAL(Result::Queued, r.c.claim("s3cr3t", 60000));
  TEST_ASSERT_EQUAL(Result::Queued, r.c.claim(nullptr, 1000));
  r.c.loop();
  TEST_ASSERT_EQUAL_UINT(2, r.t.published.size());
  TEST_ASSERT_EQUAL_STRING("v1/devices/me/claim", r.t.published[0].topic.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"secretKey\":\"s3cr3t\",\"durationMs\":60000}", r.t.published[0].payload.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"durationMs\":1000}", r.t.published[1].payload.c_str());
}
```

- [ ] **Step 2: Run, expect FAIL** (no member `cancelPending`).
- [ ] **Step 3: Implement.** `cancelPending` = `take` under the lock without firing. `claim` encodes with `codec::claim` into a 128-byte stack buffer and enqueues `RecordClass::Control`. Create `ClientRig.h`:

```cpp
// Client wired to a fake transport and clock, for module tests.
#pragma once

#include <unity.h>

#include <vector>

#include "Fakes.h"
#include "vl/Client.h"

namespace vltest {

struct ClientRig {
  FakeTransport t;
  FakeClock clock;
  vl::RingOutbox<4096> outbox;
  vl::Client c;
  vl::Config cfg;

  ClientRig() : c(t, outbox) {
    c.setClock(clock);
    cfg.host = "console.viralink.ir";
    cfg.auth = vl::AccessToken("TOKEN");
    cfg.ratePerSec = 0;
  }
  void connect() {
    TEST_ASSERT_EQUAL(vl::Result::Ok, c.begin(cfg));
    c.loop();
    t.connack(0);
    t.subackAll();
    TEST_ASSERT_EQUAL(vl::State::Online, c.state());
  }
  // Runs the loop twice (module ticks enqueue after the drain), returns what
  // was published and acknowledges it.
  std::vector<Published> flush() {
    t.published.clear();
    c.loop();
    c.loop();
    std::vector<Published> out = t.published;
    t.pubackAll();
    t.published.clear();
    return out;
  }
};

}  // namespace vltest
```

- [ ] **Step 4: Run, expect PASS** — `pio test -e native -f test_client`.

### Task 4: Server-side RPC

**Files:** Create `src/vl/api/Rpc.h`, `src/vl/api/Rpc.cpp`, `test/test_rpc/test_main.cpp`; modify `Config.h`.

**Consumes:** `JsonValue`, `codec::rpcError`, `Client::enqueue`, `Client::lock()`.

**Produces:**

```cpp
class RpcRequest {
 public:
  uint32_t id() const;
  const char* method() const;
  JsonValue params() const;   // valid only during the handler
};
class RpcReply {
 public:
  JsonWriter& json();               // writes the reply body; send() publishes it
  Result send();                    // "{}" when nothing was written
  Result send(const char* json);
  Result error(const char* message);  // {"error":message}
  bool sent() const;
};
typedef void (*RpcHandler)(const RpcRequest& req, RpcReply& reply, void* ctx);
class RpcBase : public IApiModule {
 public:
  Result on(const char* method, RpcHandler fn, void* ctx = nullptr);  // re-registering replaces
};
template <size_t N> class Rpc : public RpcBase;
```

Error replies: `bad request` (invalid JSON or no string `method`), `unknown method: <name>`, `reply too large` (a send failed with TooLarge), `no reply`. A second `send()` returns `Rejected`.

- [ ] **Step 1: Failing tests** (`test/test_rpc/test_main.cpp`)

```cpp
// Server-side and client-side RPC over the fake transport.
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include <string>

#include "ClientRig.h"
#include "vl/api/Rpc.h"

using namespace vl;
using vltest::ClientRig;
using vltest::Published;

void setUp() {}
void tearDown() {}

namespace {

struct RpcRig : ClientRig {
  Rpc<4> rpc;
  RpcRig() { c.use(rpc); }
  std::string reply(uint32_t id, const char* body) {
    char topic[64];
    snprintf(topic, sizeof(topic), "v1/devices/me/rpc/request/%lu", static_cast<unsigned long>(id));
    t.message(topic, body);
    std::vector<Published> p = flush();
    TEST_ASSERT_EQUAL_UINT(1, p.size());
    snprintf(topic, sizeof(topic), "v1/devices/me/rpc/response/%lu", static_cast<unsigned long>(id));
    TEST_ASSERT_EQUAL_STRING(topic, p[0].topic.c_str());
    return p[0].payload;
  }
};

void onSetRelay(const RpcRequest& req, RpcReply& reply, void* ctx) {
  bool* relay = static_cast<bool*>(ctx);
  *relay = req.params()["on"].asBool();
  reply.json().beginObject().key("on").value(*relay).endObject();
  reply.send();
}

void ignore(const RpcRequest&, RpcReply&, void*) {}

Result secondSend = Result::Ok;
void sendTwice(const RpcRequest&, RpcReply& reply, void*) {
  reply.send("{\"t\":1}");
  secondSend = reply.send("{\"t\":2}");
}

void busy(const RpcRequest&, RpcReply& reply, void*) { reply.error("busy"); }

void huge(const RpcRequest&, RpcReply& reply, void*) {
  std::string big(VL_RPC_REPLY_SIZE, 'x');
  reply.json().beginObject().key("k").value(big.c_str()).endObject();
  TEST_ASSERT_EQUAL(Result::TooLarge, reply.send());
}

}  // namespace

static void test_subscribes_rpc_topics() {
  RpcRig r;
  r.c.begin(r.cfg);
  r.c.loop();
  r.t.connack(0);
  bool req = false, resp = false;
  for (size_t i = 0; i < r.t.subscribed.size(); ++i) {
    req |= r.t.subscribed[i] == "v1/devices/me/rpc/request/+";
    resp |= r.t.subscribed[i] == "v1/devices/me/rpc/response/+";
  }
  TEST_ASSERT_TRUE(req && resp);
}

static void test_dispatches_and_replies() {
  RpcRig r;
  bool relay = false;
  TEST_ASSERT_EQUAL(Result::Ok, r.rpc.on("setRelay", onSetRelay, &relay));
  r.connect();
  TEST_ASSERT_EQUAL_STRING("{\"on\":true}",
                           r.reply(42, "{\"method\":\"setRelay\",\"params\":{\"on\":true}}").c_str());
  TEST_ASSERT_TRUE(relay);
}

static void test_error_replies() {
  RpcRig r;
  r.rpc.on("ignore", ignore);
  r.rpc.on("busy", busy);
  r.rpc.on("huge", huge);
  r.connect();
  TEST_ASSERT_EQUAL_STRING("{\"error\":\"unknown method: nope\"}", r.reply(7, "{\"method\":\"nope\"}").c_str());
  TEST_ASSERT_EQUAL_STRING("{\"error\":\"bad request\"}", r.reply(8, "not json").c_str());
  TEST_ASSERT_EQUAL_STRING("{\"error\":\"bad request\"}", r.reply(9, "{\"params\":1}").c_str());
  TEST_ASSERT_EQUAL_STRING("{\"error\":\"no reply\"}", r.reply(10, "{\"method\":\"ignore\"}").c_str());
  TEST_ASSERT_EQUAL_STRING("{\"error\":\"busy\"}", r.reply(11, "{\"method\":\"busy\"}").c_str());
  TEST_ASSERT_EQUAL_STRING("{\"error\":\"reply too large\"}", r.reply(12, "{\"method\":\"huge\"}").c_str());
}

static void test_send_once() {
  RpcRig r;
  r.rpc.on("twice", sendTwice);
  r.connect();
  TEST_ASSERT_EQUAL_STRING("{\"t\":1}", r.reply(1, "{\"method\":\"twice\"}").c_str());
  TEST_ASSERT_EQUAL(Result::Rejected, secondSend);
}

static void test_reply_jumps_queue() {
  RpcRig r;
  r.rpc.on("busy", busy);
  r.connect();
  for (int i = 0; i < 3; ++i) r.c.telemetry().add("i", i).send();
  r.t.message("v1/devices/me/rpc/request/5", "{\"method\":\"busy\"}");
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(4, p.size());
  TEST_ASSERT_EQUAL_STRING("v1/devices/me/rpc/response/5", p[0].topic.c_str());
}

static void test_registry() {
  Rpc<2> rpc;
  TEST_ASSERT_EQUAL(Result::Ok, rpc.on("a", ignore));
  TEST_ASSERT_EQUAL(Result::Ok, rpc.on("b", ignore));
  TEST_ASSERT_EQUAL(Result::NoMemory, rpc.on("c", ignore));
  TEST_ASSERT_EQUAL(Result::Ok, rpc.on("a", busy));  // replaces
  std::string longName(VL_RPC_METHOD_SIZE, 'm');
  TEST_ASSERT_EQUAL(Result::Rejected, rpc.on(longName.c_str(), ignore));
  TEST_ASSERT_EQUAL(Result::Rejected, rpc.on("a", nullptr));
  TEST_ASSERT_EQUAL(Result::Rejected, rpc.on(nullptr, ignore));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_subscribes_rpc_topics);
  RUN_TEST(test_dispatches_and_replies);
  RUN_TEST(test_error_replies);
  RUN_TEST(test_send_once);
  RUN_TEST(test_reply_jumps_queue);
  RUN_TEST(test_registry);
  return UNITY_END();
}
```

- [ ] **Step 2: Run, expect FAIL** (`vl/api/Rpc.h` not found).
- [ ] **Step 3: Implement.** `RpcBase(Method* table, size_t cap)` holds the table pointer; `Rpc<N>` owns `Method table_[N]`. `handle(RpcRequest topic)`: parse, copy the method name, look up, build an `RpcReply` over the module's `VL_RPC_REPLY_SIZE` buffer, call the handler, then send the automatic error if `!reply.sent()`. Replies enqueue `OutTopic::RpcResponse`, `arg = id`, `RecordClass::Priority`.
- [ ] **Step 4: Run, expect PASS** — `pio test -e native -f test_rpc`.

### Task 5: Client-side RPC

**Files:** Modify `src/vl/api/Rpc.h/.cpp`, `test/test_rpc/test_main.cpp`.

**Produces:**

```cpp
typedef void (*RpcResultCb)(Result r, JsonValue response, void* ctx);  // response Invalid unless Ok
Result RpcBase::call(const char* method, const char* paramsJson, RpcResultCb cb, void* ctx,
                     uint32_t timeoutMs = VL_RPC_TIMEOUT_MS);
```

Thread-safe (slot table under `client.lock()`); `NoMemory` when `VL_RPC_MAX_CALLS` calls are pending; `Rejected` for a null method or params that are not valid JSON; `NotReady` before `use()`.

- [ ] **Step 1: Failing tests** (add, register in `main`)

```cpp
namespace {
struct CallLog {
  int n = 0;
  Result r = Result::Ok;
  long long time = -1;
} calls;
void onCall(Result res, JsonValue v, void*) {
  ++calls.n;
  calls.r = res;
  calls.time = v["time"].asInt(-1);
}
}  // namespace

static void test_call_publishes_and_delivers_response() {
  RpcRig r;
  calls = CallLog();
  r.connect();
  TEST_ASSERT_EQUAL(Result::Queued, r.rpc.call("getTime", "{\"tz\":0}", onCall, nullptr));
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(1, p.size());
  const char* prefix = "v1/devices/me/rpc/request/";
  TEST_ASSERT_EQUAL_MEMORY(prefix, p[0].topic.c_str(), strlen(prefix));
  TEST_ASSERT_EQUAL_STRING("{\"method\":\"getTime\",\"params\":{\"tz\":0}}", p[0].payload.c_str());
  std::string resp = "v1/devices/me/rpc/response/" + p[0].topic.substr(strlen(prefix));
  r.t.message(resp.c_str(), "{\"time\":123}");
  r.t.message(resp.c_str(), "{\"time\":456}");  // duplicate: ignored
  TEST_ASSERT_EQUAL(1, calls.n);
  TEST_ASSERT_EQUAL(Result::Ok, calls.r);
  TEST_ASSERT_EQUAL_INT(123, static_cast<int>(calls.time));
}

static void test_call_times_out_and_frees_slot() {
  RpcRig r;
  calls = CallLog();
  r.connect();
  for (int i = 0; i < VL_RPC_MAX_CALLS; ++i) {
    TEST_ASSERT_EQUAL(Result::Queued, r.rpc.call("x", nullptr, onCall, nullptr, 500));
  }
  TEST_ASSERT_EQUAL(Result::NoMemory, r.rpc.call("x", nullptr, onCall, nullptr, 500));
  r.clock.advance(500);
  r.c.loop();
  TEST_ASSERT_EQUAL(VL_RPC_MAX_CALLS, calls.n);
  TEST_ASSERT_EQUAL(Result::Timeout, calls.r);
  TEST_ASSERT_EQUAL_INT(-1, static_cast<int>(calls.time));
  TEST_ASSERT_EQUAL(Result::Queued, r.rpc.call("x", nullptr, onCall, nullptr, 500));
}

static void test_call_validates() {
  RpcRig r;
  TEST_ASSERT_EQUAL(Result::Rejected, r.rpc.call(nullptr, nullptr, onCall, nullptr));
  TEST_ASSERT_EQUAL(Result::Rejected, r.rpc.call("x", "not json", onCall, nullptr));
  Rpc<1> detached;
  TEST_ASSERT_EQUAL(Result::NotReady, detached.call("x", nullptr, onCall, nullptr));
}
```

- [ ] **Step 2: Run, expect FAIL** (no member `call`).
- [ ] **Step 3: Implement.** Claim a `Call{used, cb, ctx, self}` slot under the lock, `id = client.addPending(timeout, thunk, &slot)`, encode with `codec::rpcRequest` into a `VL_RPC_REPLY_SIZE` stack buffer, enqueue `OutTopic::RpcRequest`, `arg = id`, `RecordClass::Control`; on failure `cancelPending` and free the slot. `handle(RpcResponse)` calls `client.resolvePending(t.id, …)`. The thunk copies cb/ctx, frees the slot under the lock, then calls `cb` with `JsonValue::parse` of the payload.
- [ ] **Step 4: Run, expect PASS.**

### Task 5a: Module lifetime (found by UBSan in Task 5)

A module destroyed before the client left the router and the correlator pointing at it; a client destroyed
first left modules with a dangling pointer. Both orders must be safe.

**Produces:** `class ApiModule : public IApiModule` (holds `client_`, detaches in its destructor);
`virtual void IApiModule::onDetach()`; `void Client::detach(IApiModule&)` (drops the module's pending requests
without firing them, unroutes it, calls `onDetach`); `~Client` detaches every module after `end()`;
`Correlator::add(…, const void* owner)` and `removeOwner(owner)`; `Router::remove(IApiModule&)`.

- [ ] **Step 1: Failing tests** (`test/test_rpc`)

```cpp
static void test_module_destroyed_first_drops_its_calls() {
  ClientRig rig;
  calls = CallLog();
  {
    Rpc<1> rpc;
    rig.c.use(rpc);
    rig.connect();
    TEST_ASSERT_EQUAL(Result::Queued, rpc.call("x", nullptr, onCall, nullptr, 500));
  }
  rig.clock.advance(1000);
  rig.c.loop();
  rig.t.message("v1/devices/me/rpc/request/1", "{\"method\":\"x\"}");
  TEST_ASSERT_EQUAL(0, calls.n);
}

static void test_client_destroyed_first_leaves_module_safe() {
  Rpc<1> rpc;
  calls = CallLog();
  {
    ClientRig rig;
    rig.c.use(rpc);
    rig.connect();
    TEST_ASSERT_EQUAL(Result::Queued, rpc.call("x", nullptr, onCall, nullptr, 500));
  }
  TEST_ASSERT_EQUAL(1, calls.n);
  TEST_ASSERT_EQUAL(Result::NotReady, calls.r);
  TEST_ASSERT_EQUAL(Result::NotReady, rpc.call("x", nullptr, onCall, nullptr));
}
```

- [ ] **Step 2: Run, expect FAIL** (ASan/UBSan report).
- [ ] **Step 3: Implement** as listed under Produces; `RpcBase` derives from `ApiModule`.
- [ ] **Step 4: Run, expect PASS**, full suite.

### Task 6: Attribute request and shared-attribute updates

**Files:** Create `src/vl/api/Attributes.h`, `src/vl/api/Attributes.cpp`, `test/test_attributes/test_main.cpp`.

**Produces:**

```cpp
typedef void (*AttrUpdateCb)(const char* key, JsonValue value, void* ctx);  // value Invalid = deleted
typedef void (*AttrResponseCb)(Result r, JsonValue client, JsonValue shared, void* ctx);
class SharedAttributesBase : public IApiModule {
 public:
  Result onUpdate(const char* keys, AttrUpdateCb cb, void* ctx = nullptr);  // keys "a,b"; null = every key
  Result request(const char* clientKeys, const char* sharedKeys, AttrResponseCb cb, void* ctx,
                 uint32_t timeoutMs = VL_ATTR_TIMEOUT_MS);
};
template <size_t N> class SharedAttributes : public SharedAttributesBase;
```

- [ ] **Step 1: Failing tests** (`test/test_attributes/test_main.cpp`)

```cpp
// Attribute requests, shared-attribute updates and binding, client-attribute
// reporting.
#include <math.h>
#include <string.h>
#include <unity.h>

#include <string>
#include <vector>

#include "ClientRig.h"
#include "vl/api/Attributes.h"

using namespace vl;
using vltest::ClientRig;
using vltest::Published;

void setUp() {}
void tearDown() {}

namespace {

struct AttrRig : ClientRig {
  SharedAttributes<4> shared;
  ClientAttributes<4> attrs;
  AttrRig() { c.use(shared, attrs); }
  void update(const char* json) { t.message("v1/devices/me/attributes", json); }
};

std::vector<std::string> seenA, seenB;
void record(std::vector<std::string>& out, const char* key, JsonValue v) {
  out.push_back(std::string(key) + "=" + (v.valid() ? std::string(v.raw(), v.rawSize()) : std::string("<deleted>")));
}
void onA(const char* key, JsonValue v, void*) { record(seenA, key, v); }
void onB(const char* key, JsonValue v, void*) { record(seenB, key, v); }

struct ResponseLog {
  int n = 0;
  Result r = Result::Ok;
  std::string fw, mode;
} resp;
void onResponse(Result r, JsonValue client, JsonValue shared, void*) {
  ++resp.n;
  resp.r = r;
  char buf[16] = "";
  client["fw"].copyString(buf, sizeof(buf));
  resp.fw = buf;
  buf[0] = '\0';
  shared["mode"].copyString(buf, sizeof(buf));
  resp.mode = buf;
}

std::string requestId(const Published& p) {
  const char* prefix = "v1/devices/me/attributes/request/";
  TEST_ASSERT_EQUAL_MEMORY(prefix, p.topic.c_str(), strlen(prefix));
  return p.topic.substr(strlen(prefix));
}

}  // namespace

static void test_subscribes_attribute_topics() {
  AttrRig r;
  r.c.begin(r.cfg);
  r.c.loop();
  r.t.connack(0);
  bool upd = false, res = false;
  for (size_t i = 0; i < r.t.subscribed.size(); ++i) {
    upd |= r.t.subscribed[i] == "v1/devices/me/attributes";
    res |= r.t.subscribed[i] == "v1/devices/me/attributes/response/+";
  }
  TEST_ASSERT_TRUE(upd && res);
}

static void test_update_handlers_per_key_and_deletion() {
  AttrRig r;
  seenA.clear();
  seenB.clear();
  TEST_ASSERT_EQUAL(Result::Ok, r.shared.onUpdate("mode,interval", onA));
  TEST_ASSERT_EQUAL(Result::Ok, r.shared.onUpdate(nullptr, onB));
  r.connect();
  r.update("{\"mode\":\"eco\",\"other\":1}");
  TEST_ASSERT_EQUAL_UINT(1, seenA.size());
  TEST_ASSERT_EQUAL_STRING("mode=\"eco\"", seenA[0].c_str());
  TEST_ASSERT_EQUAL_UINT(2, seenB.size());
  TEST_ASSERT_EQUAL_STRING("other=1", seenB[1].c_str());
  r.update("{\"deleted\":[\"interval\",\"x\"]}");
  TEST_ASSERT_EQUAL_UINT(2, seenA.size());
  TEST_ASSERT_EQUAL_STRING("interval=<deleted>", seenA[1].c_str());
  TEST_ASSERT_EQUAL_UINT(4, seenB.size());
  r.update("garbage");  // ignored
  TEST_ASSERT_EQUAL_UINT(4, seenB.size());
}

static void test_request_and_response() {
  AttrRig r;
  resp = ResponseLog();
  r.connect();
  TEST_ASSERT_EQUAL(Result::Queued, r.shared.request("fw,door", "mode", onResponse, nullptr));
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(1, p.size());
  TEST_ASSERT_EQUAL_STRING("{\"clientKeys\":\"fw,door\",\"sharedKeys\":\"mode\"}", p[0].payload.c_str());
  std::string topic = "v1/devices/me/attributes/response/" + requestId(p[0]);
  r.t.message(topic.c_str(), "{\"client\":{\"fw\":\"1.0\"},\"shared\":{\"mode\":\"eco\"}}");
  TEST_ASSERT_EQUAL(1, resp.n);
  TEST_ASSERT_EQUAL(Result::Ok, resp.r);
  TEST_ASSERT_EQUAL_STRING("1.0", resp.fw.c_str());
  TEST_ASSERT_EQUAL_STRING("eco", resp.mode.c_str());
}

static void test_request_timeout_and_validation() {
  AttrRig r;
  resp = ResponseLog();
  r.connect();
  TEST_ASSERT_EQUAL(Result::Rejected, r.shared.request(nullptr, nullptr, onResponse, nullptr));
  TEST_ASSERT_EQUAL(Result::Queued, r.shared.request(nullptr, "mode", onResponse, nullptr, 300));
  r.clock.advance(300);
  r.c.loop();
  TEST_ASSERT_EQUAL(1, resp.n);
  TEST_ASSERT_EQUAL(Result::Timeout, resp.r);
}

static void test_update_handler_capacity() {
  SharedAttributes<1> s;
  TEST_ASSERT_EQUAL(Result::Ok, s.onUpdate("a", onA));
  TEST_ASSERT_EQUAL(Result::NoMemory, s.onUpdate("b", onA));
  TEST_ASSERT_EQUAL(Result::Rejected, s.onUpdate("a", nullptr));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_subscribes_attribute_topics);
  RUN_TEST(test_update_handlers_per_key_and_deletion);
  RUN_TEST(test_request_and_response);
  RUN_TEST(test_request_timeout_and_validation);
  RUN_TEST(test_update_handler_capacity);
  return UNITY_END();
}
```

- [ ] **Step 2: Run, expect FAIL** (`vl/api/Attributes.h` not found).
- [ ] **Step 3: Implement.** Entry table `{kind, keys[VL_ATTR_KEYS_SIZE], cb, ctx, …}`; key-list match walks the comma list. Update: for each member, a `deleted` array fans out each name with an Invalid value. Request slots mirror client RPC (`VL_ATTR_MAX_REQUESTS`, `OutTopic::AttrRequest`, Control). `ClientAttributes<N>` gets an empty shell here so the test file compiles.
- [ ] **Step 4: Run, expect PASS** — `pio test -e native -f test_attributes`.

### Task 7: Shared-attribute binding

**Produces** (on `SharedAttributesBase`):

```cpp
typedef void (*AttrChangedCb)(const char* key, void* ctx);
Result bind(const char* key, bool* var, AttrChangedCb changed = nullptr, void* ctx = nullptr);
Result bind(const char* key, int32_t* var, AttrChangedCb changed = nullptr, void* ctx = nullptr);
Result bind(const char* key, float* var, AttrChangedCb changed = nullptr, void* ctx = nullptr);
Result bind(const char* key, char* buf, size_t cap, AttrChangedCb changed = nullptr, void* ctx = nullptr);
```

On every Online the module requests the bound keys as `sharedKeys` (comma-joined in binding order). Values of the wrong type, or strings that do not fit, leave the variable unchanged and fire no callback.

- [ ] **Step 1: Failing tests** (add, register)

```cpp
namespace {
int changes = 0;
void onChanged(const char*, void*) { ++changes; }
}  // namespace

static void test_bind_refreshes_on_online() {
  AttrRig r;
  int32_t interval = 0;
  bool enabled = false;
  float gain = 0;
  char mode[8] = "";
  TEST_ASSERT_EQUAL(Result::Ok, r.shared.bind("interval", &interval));
  TEST_ASSERT_EQUAL(Result::Ok, r.shared.bind("enabled", &enabled));
  TEST_ASSERT_EQUAL(Result::Ok, r.shared.bind("gain", &gain));
  TEST_ASSERT_EQUAL(Result::Ok, r.shared.bind("mode", mode, sizeof(mode)));
  TEST_ASSERT_EQUAL(Result::NoMemory, r.shared.onUpdate(nullptr, onA));
  r.connect();
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(1, p.size());
  TEST_ASSERT_EQUAL_STRING("{\"sharedKeys\":\"interval,enabled,gain,mode\"}", p[0].payload.c_str());
  std::string topic = "v1/devices/me/attributes/response/" + requestId(p[0]);
  r.t.message(topic.c_str(), "{\"shared\":{\"interval\":5000,\"enabled\":true,\"gain\":1.5,\"mode\":\"eco\"}}");
  TEST_ASSERT_EQUAL_INT32(5000, interval);
  TEST_ASSERT_TRUE(enabled);
  TEST_ASSERT_TRUE(fabs(gain - 1.5f) < 1e-6);
  TEST_ASSERT_EQUAL_STRING("eco", mode);
}

static void test_bind_update_delete_and_change_callback() {
  AttrRig r;
  changes = 0;
  int32_t interval = 7;
  char mode[4] = "abc";
  r.shared.bind("interval", &interval, onChanged);
  r.shared.bind("mode", mode, sizeof(mode), onChanged);
  r.connect();
  r.update("{\"interval\":250}");
  TEST_ASSERT_EQUAL_INT32(250, interval);
  TEST_ASSERT_EQUAL(1, changes);
  r.update("{\"interval\":\"x\",\"mode\":\"toolong\"}");  // wrong type, does not fit
  TEST_ASSERT_EQUAL_INT32(250, interval);
  TEST_ASSERT_EQUAL_STRING("abc", mode);
  TEST_ASSERT_EQUAL(1, changes);
  r.update("{\"deleted\":[\"interval\",\"mode\"]}");
  TEST_ASSERT_EQUAL_INT32(0, interval);
  TEST_ASSERT_EQUAL_STRING("", mode);
  TEST_ASSERT_EQUAL(3, changes);
}

static void test_bind_validation() {
  SharedAttributes<2> s;
  int32_t v = 0;
  TEST_ASSERT_EQUAL(Result::Rejected, s.bind(nullptr, &v));
  TEST_ASSERT_EQUAL(Result::Rejected, s.bind("k", static_cast<int32_t*>(nullptr)));
  TEST_ASSERT_EQUAL(Result::Rejected, s.bind("k", static_cast<char*>(nullptr), 4));
  std::string longKey(VL_ATTR_KEYS_SIZE, 'k');
  TEST_ASSERT_EQUAL(Result::Rejected, s.bind(longKey.c_str(), &v));
}
```

- [ ] **Step 2: Run, expect FAIL** (no matching `bind`).
- [ ] **Step 3: Implement.** Binding entries share the table with handlers; `onOnline` builds the key list into a stack buffer of `VL_ATTR_KEYS_SIZE * 2` bytes and issues `request(nullptr, keys, refreshThunk, this)`. Apply = set from `JsonValue` by kind: `Bool`→`asBool`, `Number`→`asInt`/`asDouble`, `String`→`copyString` into a scratch then copy.
- [ ] **Step 4: Run, expect PASS.**

### Task 8: Client-attribute reporting

**Produces:**

```cpp
template <size_t N> class ClientAttributes : public ClientAttributesBase;
Result ClientAttributesBase::report(const char* key, const bool* var, uint32_t minIntervalMs = 0);
Result ClientAttributesBase::report(const char* key, const int32_t* var, uint32_t minIntervalMs = 0);
Result ClientAttributesBase::report(const char* key, const float* var, uint32_t minIntervalMs = 0);
```

The first tick reports every variable; afterwards a variable is published only when it changed and `minIntervalMs` has passed since its last report. A failed enqueue retries on the next tick.

- [ ] **Step 1: Failing tests** (add, register)

```cpp
static void test_report_on_change_with_min_interval() {
  AttrRig r;
  int32_t door = 0;
  bool on = false;
  float gain = 1.5f;
  TEST_ASSERT_EQUAL(Result::Ok, r.attrs.report("door", &door, 1000));
  TEST_ASSERT_EQUAL(Result::Ok, r.attrs.report("on", &on));
  TEST_ASSERT_EQUAL(Result::Ok, r.attrs.report("gain", &gain));
  r.connect();
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(3, p.size());
  TEST_ASSERT_EQUAL_STRING("v1/devices/me/attributes", p[0].topic.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"door\":0}", p[0].payload.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"on\":false}", p[1].payload.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"gain\":1.5}", p[2].payload.c_str());
  TEST_ASSERT_EQUAL_UINT(0, r.flush().size());  // unchanged
  door = 1;
  on = true;
  p = r.flush();
  TEST_ASSERT_EQUAL_UINT(1, p.size());  // door waits for its interval
  TEST_ASSERT_EQUAL_STRING("{\"on\":true}", p[0].payload.c_str());
  r.clock.advance(999);
  TEST_ASSERT_EQUAL_UINT(0, r.flush().size());
  r.clock.advance(1);
  p = r.flush();
  TEST_ASSERT_EQUAL_UINT(1, p.size());
  TEST_ASSERT_EQUAL_STRING("{\"door\":1}", p[0].payload.c_str());
}

static void test_report_capacity_and_validation() {
  ClientAttributes<1> a;
  int32_t v = 0;
  TEST_ASSERT_EQUAL(Result::Rejected, a.report(nullptr, &v));
  TEST_ASSERT_EQUAL(Result::Rejected, a.report("k", static_cast<const int32_t*>(nullptr)));
  TEST_ASSERT_EQUAL(Result::Ok, a.report("k", &v));
  TEST_ASSERT_EQUAL(Result::NoMemory, a.report("j", &v));
}
```

- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement** in `tick(nowMs)` with `client->attributes<64>().add(key, value).send()`; update the last value and time only when `ok(result)`.
- [ ] **Step 4: Run, expect PASS.**

### Task 9: Public header, example, docs, gate

**Files:** Modify `src/ViraLink.h`, `README.md`, `library.json`, `CMakeLists.txt` (if sources are listed); create `examples/rpc_and_attributes/rpc_and_attributes.ino`.

- [ ] **Step 1:** Include `vl/api/Rpc.h`, `vl/api/Attributes.h`, `vl/core/JsonReader.h` from `ViraLink.h`.
- [ ] **Step 2:** Example: `Rpc<4>` with `setRelay`, `SharedAttributes<4>` binding `interval` and `mode`, `ClientAttributes<2>` reporting a door input, scheduled telemetry using the bound interval.
- [ ] **Step 3:** README sections for RPC, attributes, claiming; add the example to `library.json`.
- [ ] **Step 4: Gate** — `pio test -e native` all pass; `scripts/build_examples.sh` passes.
- [ ] **Step 5:** Tick Phase 2 in `2026-10-25-roadmap.md`.
