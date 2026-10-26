# Phase 5a: Key-Value Store and Provisioning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `vl::Provisioning` lets a device create itself on ThingsBoard with a device profile's provision key and
secret, for every credentials type (server-generated access token, device access token, MQTT basic, X.509). It saves
the returned credentials through a new `IKeyValueStore` and starts the normal session with them; later boots skip
straight to that session. ESP32 gets `NvsStore`.

**Architecture:** Phase 5 covers five independent subsystems, so it has one plan each: 5a (this plan: key-value store
and provisioning), 5b health attributes, 5c persistent outbox, 5d TLS provider and ATECC608B, 5e network manager.
Provisioning runs as an *exclusive session* of the existing client: `Client::beginExclusive()` restricts the router to
one module (`Router::setSolo`), so only `/provision/response` is subscribed, only that module sees Online, `online()`
stays false and the outbox is not drained. The session no longer waits for the `/provision/response` SUBACK, which
ThingsBoard 3.6-3.8 never send. The request is written straight into the client's transmit buffer and published
(`Client::publishNow`). On success the module saves the credentials, detaches itself and calls `setCredentials()`,
which leaves the exclusive mode and reconnects with the normal subscription registry.

**Tech Stack:** C++11 (`-std=gnu++11`), PlatformIO `native` + Unity, ASan/UBSan, `-Werror`; ESP-IDF `nvs_flash`.

## Global Constraints

- C++11 in `src/`; no heap, `std::string`, `std::function` or exceptions after `begin()`.
- Capacities are `VL_*` macros in `Config.h`; strings kept beyond a call are copied into fixed buffers, except the
  X.509 certificate PEM, which, like `PemTls`, must outlive its user.
- Callbacks are function pointers plus `void* ctx`, fired on the client task with no lock held.
- Hold the client lock only around outbox and table access: never during network I/O or a callback.
- Every payload shape lives in `core/Codec` with a golden-string test.
- Provisioning compiles only when `VL_ENABLE_PROVISION` is set (off in the Lite profile).
- Portable to every target: in `src/` outside `platform/`, no C++ standard library headers, no `printf` of floats or
  `%ll`/`%z`, no identifiers that Arduino.h defines as macros.
- Gate: `pio test -e native`, `scripts/check_portability.sh` and `scripts/build_examples.sh` pass.
- Commits: only when the user asks.

## Wire format (ThingsBoard device provisioning over MQTT)

| Direction | Topic | Payload |
| --- | --- | --- |
| connect | | username `provision`, no password; client ID as configured |
| subscribe | `/provision/response` | no SUBACK on 3.6-3.8 |
| out | `/provision/request` | `{"deviceName":"Meter-7","provisionDeviceKey":"KEY","provisionDeviceSecret":"SECRET"}` (name omitted when null) |
| | | device token: `…,"credentialsType":"ACCESS_TOKEN","token":"T"}` |
| | | MQTT basic: `…,"credentialsType":"MQTT_BASIC","username":"u","password":"p","clientId":"c"}` |
| | | X.509: `…,"credentialsType":"X509_CERTIFICATE","hash":"<certificate PEM>"}` |
| in | `/provision/response` | `{"credentialsType":"ACCESS_TOKEN","credentialsValue":"T","status":"SUCCESS"}` |
| | | `{"credentialsType":"MQTT_BASIC","credentialsValue":{"clientId":…,"userName":…,"password":…},"status":"SUCCESS"}` |
| | | `{"credentialsType":"X509_CERTIFICATE","credentialsValue":"<cert>","status":"SUCCESS"}` |
| | | `{"errorMsg":"Provision data was not found!","status":"NOT_FOUND"}` (or `FAILURE`) |

The response shapes follow ThingsBoard's `JsonConverter.toJson(ProvisionDeviceResponseMsg)`. Older documentation
spells the status key `provisionDeviceStatus`; both are accepted. The Phase 6 integration suite checks this against
3.6 (no SUBACK) and 4.x.

## Decisions

- **One request per session, one deadline overall.** Each time the provisioning session reaches Online it publishes
  the request once. If the link drops, the session's usual backoff reconnects and the request goes out again. The whole
  attempt fails with `Timeout` after `VL_PROVISION_TIMEOUT_MS` (30 s). On any failure the client is stopped
  (`end()`), and the app may call `start()` again.
- **Saved format:** one blob under key `vl_cred`: `'V'`, version 1, kind, then client ID, username and password, each
  NUL-terminated. A single `set()` keeps it atomic on NVS. A blob that fails validation is ignored, and the device
  provisions again.
- **A save failure does not fail provisioning.** The credentials are used for this boot and the error is logged. The
  next boot provisions again.
- **The module attaches and detaches itself.** `start()` attaches it; on completion it detaches, so the normal session
  never subscribes to `/provision/response`. Detaching inside `tick()` makes the client skip one module's tick once,
  which is harmless.
- **Not in this phase:** a KV store for targets other than ESP32 (Phase 6), OTA resume after reboot (it needs this
  store; it is a follow-up to Phase 4), gateway sub-device provisioning.

## File Structure

| File | Responsibility |
| --- | --- |
| `src/vl/core/Topics.h/.cpp` | `expectsSuback(InTopic)` |
| `src/vl/core/Router.h/.cpp` | `setSolo()`, `filterKind()` |
| `src/vl/core/Session.cpp` | waits only for SUBACKs the server sends |
| `src/vl/Credentials.h` | `Credentials` and its helpers, moved out of `Client.h` for the codec |
| `src/vl/Config.h` | `VL_USERNAME_SIZE`, `VL_PASSWORD_SIZE`, `VL_PROVISION_*` |
| `src/vl/core/Codec.h/.cpp` | `codec::provisionRequest` |
| `src/vl/Client.h/.cpp` | `beginExclusive()`, `publishNow()`, `online()` false while exclusive |
| `src/vl/pal/KeyValueStore.h` | `IKeyValueStore` |
| `src/vl/api/Provision.h/.cpp` | `Provisioning` |
| `src/vl/platform/esp32/EspNvs.h/.cpp` | `NvsStore` |
| `src/ViraLink.h`, `CMakeLists.txt` | includes; `nvs_flash` |
| `test/support/Fakes.h` | `MemoryStore` |
| `test/test_session`, `test/test_codec`, `test/test_provision` | tests |
| `examples/provisioning/provisioning.ino`, `library.json`, `README.md`, `test/portability/portability.ino` | example and docs |

---

### Task 1: Subscriptions without SUBACK, and the solo router

**Files:**
- Modify: `src/vl/core/Topics.h`, `src/vl/core/Topics.cpp`, `src/vl/core/Router.h`, `src/vl/core/Router.cpp`,
  `src/vl/core/Session.cpp`
- Test: `test/test_session/test_main.cpp`

**Interfaces:**
- Produces:

```cpp
bool expectsSuback(InTopic kind);          // false for InTopic::ProvisionResponse
void Router::setSolo(const IApiModule* m); // only m subscribes and receives; null = everyone; cleared by remove(m)
InTopic Router::filterKind(size_t i) const;
```

- [ ] **Step 1: Write the failing tests.** Apply to `test/test_session/test_main.cpp` (it reuses the `Counter` module
  from Phase 4):

```diff
--- a/test/test_session/test_main.cpp
+++ b/test/test_session/test_main.cpp
@@ -266,6 +266,58 @@
   TEST_ASSERT_EQUAL(1, b.seen);
 }
 
+namespace {
+class ProvisionModule : public IApiModule {
+ public:
+  int seen = 0;
+  void topics(TopicSink& s) override { s.add(InTopic::ProvisionResponse); }
+  bool handle(const Topic&, Bytes) override {
+    ++seen;
+    return true;
+  }
+};
+}  // namespace
+
+// ThingsBoard 3.6-3.8 never acknowledge /provision/response.
+static void test_provision_response_needs_no_suback() {
+  FakeTransport t;
+  Router router;
+  ProvisionModule m;
+  router.add(m);
+  Session s(t, router);
+  ConnectOptions o;
+  o.host = "broker";
+  s.start(o, 0);
+  s.connack(0, 0);
+  TEST_ASSERT_EQUAL_UINT(1, t.subscribed.size());
+  TEST_ASSERT_EQUAL_STRING("/provision/response", t.subscribed[0].c_str());
+  TEST_ASSERT_EQUAL(State::Online, s.state());
+}
+
+// The provisioning session subscribes and routes for its module only.
+static void test_solo_router() {
+  Router router;
+  Counter a;
+  ProvisionModule p;
+  router.add(a);
+  router.add(p);
+  TEST_ASSERT_EQUAL_UINT(3, router.filterCount());
+  router.setSolo(&p);
+  TEST_ASSERT_EQUAL_UINT(1, router.filterCount());
+  TEST_ASSERT_EQUAL_STRING("/provision/response", router.filter(0));
+  TEST_ASSERT_TRUE(router.filterKind(0) == InTopic::ProvisionResponse);
+  TEST_ASSERT_NULL(router.filter(1));
+  TEST_ASSERT_FALSE(router.subscribed(InTopic::AttrUpdate));
+  TEST_ASSERT_FALSE(router.dispatch("v1/devices/me/attributes", Bytes()));
+  TEST_ASSERT_TRUE(router.dispatch("/provision/response", Bytes()));
+  TEST_ASSERT_EQUAL(0, a.seen);
+  TEST_ASSERT_EQUAL(1, p.seen);
+  router.remove(p);  // removing the solo module restores everyone
+  TEST_ASSERT_EQUAL_UINT(2, router.filterCount());
+  TEST_ASSERT_TRUE(router.dispatch("v1/devices/me/attributes", Bytes()));
+  TEST_ASSERT_EQUAL(1, a.seen);
+}
+
 static void test_stop() {
   Rig r;
   r.startAndConnect(0);
@@ -293,6 +345,8 @@
   RUN_TEST(test_late_events_are_ignored);
   RUN_TEST(test_no_modules_goes_online_on_connack);
   RUN_TEST(test_attribute_updates_reach_every_subscriber);
+  RUN_TEST(test_provision_response_needs_no_suback);
+  RUN_TEST(test_solo_router);
   RUN_TEST(test_stop);
   return UNITY_END();
 }
```

- [ ] **Step 2: Run them to verify they fail.** `pio test -e native -f test_session`. Expected: compile errors,
  `'class vl::Router' has no member named 'setSolo'`.

- [ ] **Step 3: Topics.**

```diff
--- a/src/vl/core/Topics.h
+++ b/src/vl/core/Topics.h
@@ -56,4 +56,7 @@
 // Subscription filter for an incoming topic kind.
 const char* filterFor(InTopic kind);
 
+// False for /provision/response: ThingsBoard 3.6-3.8 never acknowledge it.
+bool expectsSuback(InTopic kind);
+
 }  // namespace vl
```

```diff
--- a/src/vl/core/Topics.cpp
+++ b/src/vl/core/Topics.cpp
@@ -132,4 +132,6 @@
   return kFilters[static_cast<size_t>(kind)];
 }
 
+bool expectsSuback(InTopic kind) { return kind != InTopic::ProvisionResponse; }
+
 }  // namespace vl
```

- [ ] **Step 4: Router.**

```diff
--- a/src/vl/core/Router.h
+++ b/src/vl/core/Router.h
@@ -22,10 +22,15 @@
   // Cached at add(): the module itself may be gone by the time this is read.
   bool detachesItself(size_t i) const { return selfDetach_[i]; }
 
+  // Restricts subscriptions and routing to m, an attached module (the
+  // provisioning session); null restores every module.
+  void setSolo(const IApiModule* m);
+
   // Subscription filters, in a stable order.
   size_t filterCount() const;
   const char* filter(size_t i) const;
-  bool subscribed(InTopic kind) const { return (allKinds_ & kindBit(kind)) != 0; }
+  InTopic filterKind(size_t i) const;
+  bool subscribed(InTopic kind) const { return (activeKinds() & kindBit(kind)) != 0; }
 
   // Returns true if some module consumed the message. AttrUpdate reaches
   // every subscribed module; other kinds stop at the first that consumes.
@@ -33,6 +38,7 @@
 
  private:
   static uint32_t kindBit(InTopic k) { return 1u << static_cast<uint8_t>(k); }
+  uint32_t activeKinds() const { return solo_ ? soloKinds_ : allKinds_; }
 
   class Sink : public TopicSink {
    public:
@@ -45,6 +51,8 @@
   bool selfDetach_[VL_MAX_MODULES] = {};
   uint32_t allKinds_ = 0;
   size_t count_ = 0;
+  const IApiModule* solo_ = nullptr;
+  uint32_t soloKinds_ = 0;
 };
 
 }  // namespace vl
```

```diff
--- a/src/vl/core/Router.cpp
+++ b/src/vl/core/Router.cpp
@@ -21,6 +21,7 @@
 }
 
 void Router::remove(IApiModule& m) {
+  if (solo_ == &m) setSolo(nullptr);
   size_t i = 0;
   while (i < count_ && modules_[i] != &m) ++i;
   if (i == count_) return;
@@ -37,21 +38,36 @@
   for (i = 0; i < count_; ++i) allKinds_ |= kinds_[i];
 }
 
+void Router::setSolo(const IApiModule* m) {
+  solo_ = nullptr;
+  soloKinds_ = 0;
+  for (size_t i = 0; m && i < count_; ++i) {
+    if (modules_[i] != m) continue;
+    solo_ = m;
+    soloKinds_ = kinds_[i];
+  }
+}
+
 size_t Router::filterCount() const {
   size_t n = 0;
   for (uint8_t k = 1; k < static_cast<uint8_t>(InTopic::Count); ++k) {
-    if (allKinds_ & (1u << k)) ++n;
+    if (activeKinds() & (1u << k)) ++n;
   }
   return n;
 }
 
-const char* Router::filter(size_t i) const {
+InTopic Router::filterKind(size_t i) const {
   for (uint8_t k = 1; k < static_cast<uint8_t>(InTopic::Count); ++k) {
-    if (!(allKinds_ & (1u << k))) continue;
-    if (i == 0) return filterFor(static_cast<InTopic>(k));
+    if (!(activeKinds() & (1u << k))) continue;
+    if (i == 0) return static_cast<InTopic>(k);
     --i;
   }
-  return nullptr;
+  return InTopic::Unknown;
+}
+
+const char* Router::filter(size_t i) const {
+  InTopic k = filterKind(i);
+  return k == InTopic::Unknown ? nullptr : filterFor(k);
 }
 
 bool Router::dispatch(const char* topic, Bytes payload) {
@@ -65,6 +81,7 @@
   bool all = t.kind == InTopic::AttrUpdate;
   bool consumed = false;
   for (size_t i = 0; i < count_; ++i) {
+    if (solo_ && modules_[i] != solo_) continue;
     if (!(kinds_[i] & kindBit(t.kind)) || !modules_[i]->handle(t, payload)) continue;
     consumed = true;
     if (!all) return true;
```

- [ ] **Step 5: Session.**

```diff
--- a/src/vl/core/Session.cpp
+++ b/src/vl/core/Session.cpp
@@ -108,7 +108,10 @@
     case 0: {
       size_t n = router_.filterCount();
       if (n > 32) n = 32;
-      pendingSubacks_ = n == 32 ? 0xFFFFFFFFu : (1u << n) - 1u;
+      pendingSubacks_ = 0;
+      for (size_t i = 0; i < n; ++i) {
+        if (expectsSuback(router_.filterKind(i))) pendingSubacks_ |= 1u << i;
+      }
       deadline_ = nowMs + timing_.subscribeTimeoutMs;
       enter(State::Subscribing);
       for (size_t i = 0; i < n; ++i) transport_.subscribe(router_.filter(i), 1);
```

- [ ] **Step 6: Run the tests.** `pio test -e native -f test_session`. Expected: 18 test cases, all PASSED.

---

### Task 2: Credentials header, size macros and the request codec

**Files:**
- Create: `src/vl/Credentials.h`
- Modify: `src/vl/Client.h` (Credentials moved out; buffer sizes), `src/vl/Config.h`, `src/vl/core/Codec.h`,
  `src/vl/core/Codec.cpp`
- Test: `test/test_codec/test_main.cpp`

**Interfaces:**
- Produces:

```cpp
// vl/Credentials.h: struct Credentials, AccessToken(), MqttBasic(), X509(), unchanged, now included by Client.h
void codec::provisionRequest(JsonWriter& w, const char* deviceName, const char* key, const char* secret,
                             const Credentials& creds, const char* certPem);
// Config.h: VL_USERNAME_SIZE 96, VL_PASSWORD_SIZE 64, VL_PROVISION_NAME_SIZE 64, VL_PROVISION_KEY_SIZE 64,
// VL_PROVISION_TIMEOUT_MS 30000
```

- [ ] **Step 1: Write the failing test.**

```diff
--- a/test/test_codec/test_main.cpp
+++ b/test/test_codec/test_main.cpp
@@ -206,6 +206,40 @@
   }
 }
 
+static void test_provision_requests() {
+  char buf[256];
+  {
+    JsonWriter w(buf, sizeof(buf));
+    codec::provisionRequest(w, "Meter-7", "KEY", "SECRET", Credentials(), nullptr);
+    TEST_ASSERT_EQUAL_STRING(
+        "{\"deviceName\":\"Meter-7\",\"provisionDeviceKey\":\"KEY\",\"provisionDeviceSecret\":\"SECRET\"}", buf);
+  }
+  {
+    JsonWriter w(buf, sizeof(buf));
+    codec::provisionRequest(w, nullptr, "KEY", "SECRET", AccessToken("tok"), nullptr);
+    TEST_ASSERT_EQUAL_STRING(
+        "{\"provisionDeviceKey\":\"KEY\",\"provisionDeviceSecret\":\"SECRET\",\"credentialsType\":\"ACCESS_TOKEN\","
+        "\"token\":\"tok\"}",
+        buf);
+  }
+  {
+    JsonWriter w(buf, sizeof(buf));
+    codec::provisionRequest(w, "M", "K", "S", MqttBasic("cid", "user", "pass"), nullptr);
+    TEST_ASSERT_EQUAL_STRING(
+        "{\"deviceName\":\"M\",\"provisionDeviceKey\":\"K\",\"provisionDeviceSecret\":\"S\",\"credentialsType\":"
+        "\"MQTT_BASIC\",\"username\":\"user\",\"password\":\"pass\",\"clientId\":\"cid\"}",
+        buf);
+  }
+  {
+    JsonWriter w(buf, sizeof(buf));
+    codec::provisionRequest(w, "M", "K", "S", X509(), "-----BEGIN CERTIFICATE-----\nMIIB\n");
+    TEST_ASSERT_EQUAL_STRING(
+        "{\"deviceName\":\"M\",\"provisionDeviceKey\":\"K\",\"provisionDeviceSecret\":\"S\",\"credentialsType\":"
+        "\"X509_CERTIFICATE\",\"hash\":\"-----BEGIN CERTIFICATE-----\\nMIIB\\n\"}",
+        buf);
+  }
+}
+
 static std::string fmt(double v, int digits) {
   char buf[32];
   size_t n = json::formatDouble(v, digits, buf);
@@ -261,6 +295,7 @@
   RUN_TEST(test_payload_encoders);
   RUN_TEST(test_rpc_payloads);
   RUN_TEST(test_ota_payloads);
+  RUN_TEST(test_provision_requests);
   RUN_TEST(test_format_double);
   RUN_TEST(test_format_double_round_trips_floats);
   return UNITY_END();
```

- [ ] **Step 2: Run it to verify it fails.** `pio test -e native -f test_codec`. Expected: compile error,
  `'provisionRequest' is not a member of 'vl::codec'`.

- [ ] **Step 3: Create `src/vl/Credentials.h`**. Its body is moved, unchanged, from `Client.h`:

```cpp
#pragma once

#include <stdint.h>

namespace vl {

struct Credentials {
  enum class Kind : uint8_t { None, AccessToken, MqttBasic, X509 };
  Kind kind = Kind::None;
  const char* clientId = nullptr;
  const char* username = nullptr;
  const char* password = nullptr;
};

inline Credentials AccessToken(const char* token) {
  Credentials c;
  c.kind = Credentials::Kind::AccessToken;
  c.username = token;
  return c;
}

inline Credentials MqttBasic(const char* clientId, const char* username, const char* password) {
  Credentials c;
  c.kind = Credentials::Kind::MqttBasic;
  c.clientId = clientId;
  c.username = username;
  c.password = password;
  return c;
}

// Device identified by its TLS client certificate (set Config::tls).
inline Credentials X509() {
  Credentials c;
  c.kind = Credentials::Kind::X509;
  return c;
}

}  // namespace vl
```

- [ ] **Step 4: Config.**

```diff
--- a/src/vl/Config.h
+++ b/src/vl/Config.h
@@ -49,6 +49,14 @@
 #endif
 #endif
 
+// Longest MQTT username (access token) and password + 1.
+#ifndef VL_USERNAME_SIZE
+#define VL_USERNAME_SIZE 96
+#endif
+#ifndef VL_PASSWORD_SIZE
+#define VL_PASSWORD_SIZE 64
+#endif
+
 // Default capacity of a PayloadBuilder returned by telemetry()/attributes().
 #ifndef VL_BUILDER_SIZE
 #define VL_BUILDER_SIZE (VL_PROFILE_LITE ? 96 : 256)
@@ -164,6 +172,18 @@
 #define VL_OTA_ALLOW_UNVERIFIED 0
 #endif
 
+// Provisioning: device name, provision key and secret (+ 1 each), and how
+// long the provisioning session may take before it gives up.
+#ifndef VL_PROVISION_NAME_SIZE
+#define VL_PROVISION_NAME_SIZE 64
+#endif
+#ifndef VL_PROVISION_KEY_SIZE
+#define VL_PROVISION_KEY_SIZE 64
+#endif
+#ifndef VL_PROVISION_TIMEOUT_MS
+#define VL_PROVISION_TIMEOUT_MS 30000
+#endif
+
 // Wall-clock floor (2026-01-01T00:00:00Z). Anything earlier is "clock unknown".
 #ifndef VL_TIME_FLOOR_S
 #define VL_TIME_FLOOR_S 1767225600UL
```

- [ ] **Step 5: Codec.**

```diff
--- a/src/vl/core/Codec.h
+++ b/src/vl/core/Codec.h
@@ -5,6 +5,7 @@
 #include <stddef.h>
 #include <stdint.h>
 
+#include "vl/Credentials.h"
 #include "vl/core/JsonWriter.h"
 #include "vl/core/Outbox.h"
 
@@ -39,6 +40,14 @@
 // {"fw_state":…}, plus "fw_error" when error is not null.
 void fwState(JsonWriter& w, const char* state, const char* error);
 
+// {"deviceName":…,"provisionDeviceKey":…,"provisionDeviceSecret":…} plus the
+// credentials the device chose: "credentialsType" with "token" (AccessToken),
+// "username"/"password"/"clientId" (MqttBasic) or "hash" = certPem (X509).
+// A null deviceName or credential field is omitted; Kind::None lets the
+// server generate an access token.
+void provisionRequest(JsonWriter& w, const char* deviceName, const char* key, const char* secret,
+                      const Credentials& creds, const char* certPem);
+
 // Gateway API
 void gwConnect(JsonWriter& w, const char* device, const char* type);  // type may be null
 void gwDisconnect(JsonWriter& w, const char* device);
```

```diff
--- a/src/vl/core/Codec.cpp
+++ b/src/vl/core/Codec.cpp
@@ -80,6 +80,30 @@
   w.endObject();
 }
 
+void provisionRequest(JsonWriter& w, const char* deviceName, const char* key, const char* secret,
+                      const Credentials& creds, const char* certPem) {
+  w.beginObject();
+  if (deviceName) w.key("deviceName").value(deviceName);
+  w.key("provisionDeviceKey").value(key).key("provisionDeviceSecret").value(secret);
+  switch (creds.kind) {
+    case Credentials::Kind::AccessToken:
+      w.key("credentialsType").value("ACCESS_TOKEN").key("token").value(creds.username);
+      break;
+    case Credentials::Kind::MqttBasic:
+      w.key("credentialsType").value("MQTT_BASIC");
+      if (creds.username) w.key("username").value(creds.username);
+      if (creds.password) w.key("password").value(creds.password);
+      if (creds.clientId) w.key("clientId").value(creds.clientId);
+      break;
+    case Credentials::Kind::X509:
+      w.key("credentialsType").value("X509_CERTIFICATE").key("hash").value(certPem);
+      break;
+    case Credentials::Kind::None:
+      break;
+  }
+  w.endObject();
+}
+
 void gwConnect(JsonWriter& w, const char* device, const char* type) {
   w.beginObject().key("device").value(device);
   if (type) w.key("type").value(type);
```

- [ ] **Step 6: Client.h** (Credentials removed, buffer sizes from Config, plus Task 3's additions; apply the whole
  diff in Task 3 Step 4 if you prefer one edit). For this task, remove the `Credentials` block from `Client.h`, add
  `#include "vl/Credentials.h"`, and size `username_`/`password_` with `VL_USERNAME_SIZE`/`VL_PASSWORD_SIZE`.

- [ ] **Step 7: Run the tests.** `pio test -e native`. Expected: all pass (codec 15 cases).

---

### Task 3: Exclusive session, `IKeyValueStore` and `Provisioning`

**Files:**
- Create: `src/vl/pal/KeyValueStore.h`, `src/vl/api/Provision.h`, `src/vl/api/Provision.cpp`
- Modify: `src/vl/Client.h`, `src/vl/Client.cpp`, `test/support/Fakes.h`
- Test: `test/test_provision/test_main.cpp`

**Interfaces:**
- Consumes: Task 1 (`setSolo`, no-SUBACK subscription), Task 2 (`Credentials.h`, `codec::provisionRequest`, macros).
- Produces:

```cpp
Result Client::beginExclusive(const Config& cfg, IApiModule& m);  // NotReady if m is not attached
typedef void (*PayloadFn)(JsonWriter& w, void* ctx);
Result Client::publishNow(OutTopic topic, uint32_t arg, PayloadFn fn, void* ctx);  // exclusive session only
bool Client::online() const;  // false while exclusive
class IKeyValueStore { get(key, buf, cap, len&) -> Rejected if missing, TooLarge; set(key, value, len); erase(key) };
typedef void (*ProvisionDoneCb)(Result r, const char* error, void* ctx);
class Provisioning : public ApiModule {
  explicit Provisioning(IKeyValueStore* store = nullptr);
  Result setDevice(const char* name, const char* key, const char* secret);
  Result setCredentials(const Credentials& c);
  void setCertificate(const char* certPem);
  void onDone(ProvisionDoneCb cb, void* ctx);
  Result start(Client& client, const Config& cfg);  // instead of client.begin()
  Result forget();
  const Credentials& credentials() const;
};
```

- [ ] **Step 1: Test support.** Add `MemoryStore` to `test/support/Fakes.h`:

```diff
--- a/test/support/Fakes.h
+++ b/test/support/Fakes.h
@@ -1,10 +1,14 @@
 // Test doubles: a manual clock and a scripted MQTT transport.
 #pragma once
 
+#include <string.h>
+
+#include <map>
 #include <string>
 #include <vector>
 
 #include "vl/pal/Clock.h"
+#include "vl/pal/KeyValueStore.h"
 #include "vl/transport/IMqttTransport.h"
 
 namespace vltest {
@@ -134,4 +138,28 @@
   void* closedCtx = nullptr;
 };
 
+class MemoryStore : public vl::IKeyValueStore {
+ public:
+  std::map<std::string, std::string> data;
+  bool failSet = false;
+
+  vl::Result get(const char* key, void* buf, size_t cap, size_t& len) override {
+    std::map<std::string, std::string>::const_iterator it = data.find(key);
+    if (it == data.end()) return vl::Result::Rejected;
+    if (it->second.size() > cap) return vl::Result::TooLarge;
+    memcpy(buf, it->second.data(), it->second.size());
+    len = it->second.size();
+    return vl::Result::Ok;
+  }
+  vl::Result set(const char* key, const void* value, size_t len) override {
+    if (failSet) return vl::Result::Rejected;
+    data[key].assign(static_cast<const char*>(value), len);
+    return vl::Result::Ok;
+  }
+  vl::Result erase(const char* key) override {
+    data.erase(key);
+    return vl::Result::Ok;
+  }
+};
+
 }  // namespace vltest
```

- [ ] **Step 2: Write the failing test** `test/test_provision/test_main.cpp`:

```cpp
// Provisioning: saved credentials, the exclusive provisioning session, every
// credentials type, refusals, timeouts and reconnects.
#include <string.h>
#include <unity.h>

#include <algorithm>
#include <string>
#include <vector>

#include "Fakes.h"
#include "vl/Client.h"
#include "vl/api/Provision.h"
#include "vl/api/Rpc.h"

using namespace vl;
using vltest::FakeClock;
using vltest::FakeTransport;
using vltest::MemoryStore;
using vltest::Published;

void setUp() {}
void tearDown() {}

namespace {

struct Done {
  int n = 0;
  Result r = Result::Ok;
  std::string error;
} done;

void onDone(Result r, const char* error, void*) {
  ++done.n;
  done.r = r;
  done.error = error ? error : "";
}

const char* const kOkToken = "{\"credentialsType\":\"ACCESS_TOKEN\",\"credentialsValue\":\"NEWTOKEN\",\"status\":\"SUCCESS\"}";

std::string blob(uint8_t kind, const char* cid, const char* user, const char* pass) {
  std::string b("V\x01", 2);
  b += static_cast<char>(kind);
  b += cid;
  b += '\0';
  b += user;
  b += '\0';
  b += pass;
  b += '\0';
  return b;
}

bool has(const std::vector<std::string>& v, const char* s) { return std::find(v.begin(), v.end(), s) != v.end(); }

struct ProvRig {
  FakeTransport t;
  FakeClock clock;
  RingOutbox<4096> outbox;
  Client c;
  Config cfg;
  MemoryStore store;
  Provisioning prov;
  Rpc<1> rpc;

  ProvRig() : c(t, outbox), prov(&store) {
    done = Done();
    c.setClock(clock);
    cfg.host = "console.viralink.ir";
    cfg.ratePerSec = 0;
    c.use(rpc);
    prov.onDone(onDone, nullptr);
    TEST_ASSERT_EQUAL(Result::Ok, prov.setDevice("Meter-7", "KEY", "SECRET"));
  }
  // Starts provisioning up to the request; returns the request payload.
  std::string request() {
    TEST_ASSERT_EQUAL(Result::Ok, prov.start(c, cfg));
    c.loop();
    TEST_ASSERT_EQUAL_STRING("provision", t.lastUser.c_str());
    t.connack(0);  // no SUBACK comes for /provision/response
    TEST_ASSERT_EQUAL(State::Online, c.state());
    TEST_ASSERT_FALSE(c.online());
    TEST_ASSERT_EQUAL_UINT(1, t.subscribed.size());
    TEST_ASSERT_EQUAL_STRING("/provision/response", t.subscribed[0].c_str());
    t.subscribed.clear();
    t.published.clear();
    c.loop();
    TEST_ASSERT_EQUAL_UINT(1, t.published.size());
    TEST_ASSERT_EQUAL_STRING("/provision/request", t.published[0].topic.c_str());
    return t.published[0].payload;
  }
  // Delivers the response; the normal session starts on the next loop.
  void respond(const char* json) {
    t.message("/provision/response", json);
    t.published.clear();
    c.loop();
  }
  void online() {
    t.connack(0);
    TEST_ASSERT_FALSE(has(t.subscribed, "/provision/response"));
    TEST_ASSERT_TRUE(has(t.subscribed, "v1/devices/me/rpc/request/+"));
    t.subackAll();
    TEST_ASSERT_TRUE(c.online());
  }
};

}  // namespace

static void test_saved_credentials_skip_provisioning() {
  {
    ProvRig r;
    r.store.data["vl_cred"] = blob(1, "", "SAVED", "");
    TEST_ASSERT_EQUAL(Result::Ok, r.prov.start(r.c, r.cfg));
    r.c.loop();
    TEST_ASSERT_EQUAL_STRING("SAVED", r.t.lastUser.c_str());
    r.online();
    TEST_ASSERT_TRUE(r.prov.credentials().kind == Credentials::Kind::AccessToken);
  }
  {
    ProvRig r;
    r.store.data["vl_cred"] = blob(2, "cid", "user", "pass");
    TEST_ASSERT_EQUAL(Result::Ok, r.prov.start(r.c, r.cfg));
    r.c.loop();
    TEST_ASSERT_EQUAL_STRING("cid", r.t.lastOpts.clientId);
    TEST_ASSERT_EQUAL_STRING("user", r.t.lastUser.c_str());
    TEST_ASSERT_EQUAL_STRING("pass", r.t.lastOpts.password);
  }
  // Unreadable or foreign blobs are ignored: the device provisions instead.
  const std::string bad[] = {"junk", blob(9, "", "x", ""), blob(1, "", "", ""), std::string("V\x01\x01x", 4)};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    ProvRig r;
    r.store.data["vl_cred"] = bad[i];
    r.request();
  }
}

static void test_provisions_with_server_generated_token() {
  ProvRig r;
  TEST_ASSERT_EQUAL(Result::Queued, r.c.telemetry().add("t", 1).send());  // waits for the real session
  std::string req = r.request();
  TEST_ASSERT_EQUAL_STRING(
      "{\"deviceName\":\"Meter-7\",\"provisionDeviceKey\":\"KEY\",\"provisionDeviceSecret\":\"SECRET\"}", req.c_str());
  r.t.published.clear();
  r.c.loop();
  TEST_ASSERT_EQUAL_UINT(0, r.t.published.size());  // nothing else in this session

  int connects = r.t.connectCalls;
  r.respond(kOkToken);
  TEST_ASSERT_EQUAL(1, done.n);
  TEST_ASSERT_EQUAL(Result::Ok, done.r);
  TEST_ASSERT_EQUAL(connects + 1, r.t.connectCalls);
  TEST_ASSERT_EQUAL_STRING("NEWTOKEN", r.t.lastUser.c_str());
  TEST_ASSERT_TRUE(r.store.data["vl_cred"] == blob(1, "", "NEWTOKEN", ""));
  TEST_ASSERT_EQUAL_STRING("NEWTOKEN", r.prov.credentials().username);

  r.online();
  r.t.published.clear();
  r.c.loop();
  TEST_ASSERT_EQUAL_UINT(1, r.t.published.size());
  TEST_ASSERT_EQUAL_STRING("v1/devices/me/telemetry", r.t.published[0].topic.c_str());

  // A later boot uses the saved token.
  ProvRig next;
  next.store.data = r.store.data;
  TEST_ASSERT_EQUAL(Result::Ok, next.prov.start(next.c, next.cfg));
  next.c.loop();
  TEST_ASSERT_EQUAL_STRING("NEWTOKEN", next.t.lastUser.c_str());
}

static void test_device_chosen_credentials() {
  {
    ProvRig r;
    TEST_ASSERT_EQUAL(Result::Ok, r.prov.setCredentials(AccessToken("MYTOKEN")));
    std::string req = r.request();
    TEST_ASSERT_TRUE(req.find("\"credentialsType\":\"ACCESS_TOKEN\",\"token\":\"MYTOKEN\"") != std::string::npos);
    r.respond("{\"credentialsType\":\"ACCESS_TOKEN\",\"credentialsValue\":\"MYTOKEN\",\"status\":\"SUCCESS\"}");
    TEST_ASSERT_EQUAL_STRING("MYTOKEN", r.t.lastUser.c_str());
  }
  {
    ProvRig r;
    TEST_ASSERT_EQUAL(Result::Ok, r.prov.setCredentials(MqttBasic("cid", "user", "pass")));
    std::string req = r.request();
    TEST_ASSERT_TRUE(req.find("\"credentialsType\":\"MQTT_BASIC\",\"username\":\"user\",\"password\":\"pass\","
                              "\"clientId\":\"cid\"") != std::string::npos);
    r.respond(
        "{\"credentialsType\":\"MQTT_BASIC\",\"credentialsValue\":{\"clientId\":\"cid\",\"userName\":\"user\","
        "\"password\":\"pass\"},\"status\":\"SUCCESS\"}");
    TEST_ASSERT_EQUAL(Result::Ok, done.r);
    TEST_ASSERT_EQUAL_STRING("cid", r.t.lastOpts.clientId);
    TEST_ASSERT_EQUAL_STRING("user", r.t.lastUser.c_str());
    TEST_ASSERT_EQUAL_STRING("pass", r.t.lastOpts.password);
    TEST_ASSERT_TRUE(r.store.data["vl_cred"] == blob(2, "cid", "user", "pass"));
  }
  {
    ProvRig r;
    TEST_ASSERT_EQUAL(Result::Rejected, r.prov.setCredentials(AccessToken("")));
    TEST_ASSERT_EQUAL(Result::Rejected, r.prov.setCredentials(MqttBasic(nullptr, nullptr, "pass")));
    std::string longToken(VL_USERNAME_SIZE, 'x');
    TEST_ASSERT_EQUAL(Result::TooLarge, r.prov.setCredentials(AccessToken(longToken.c_str())));
  }
}

static void test_x509() {
  PemTls tls("CA", "CERT", "KEY");
  const char* pem = "-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n";
  {
    ProvRig r;
    TEST_ASSERT_EQUAL(Result::Ok, r.prov.setCredentials(X509()));
    TEST_ASSERT_EQUAL(Result::Rejected, r.prov.start(r.c, r.cfg));  // no certificate
    r.prov.setCertificate(pem);
    TEST_ASSERT_EQUAL(Result::Rejected, r.prov.start(r.c, r.cfg));  // no Config::tls
  }
  ProvRig r;
  r.cfg.tls = &tls;
  r.prov.setCredentials(X509());
  r.prov.setCertificate(pem);
  std::string req = r.request();
  TEST_ASSERT_TRUE(req.find("\"credentialsType\":\"X509_CERTIFICATE\",\"hash\":\"-----BEGIN CERTIFICATE-----\\nMIIB") !=
                   std::string::npos);
  // Older ThingsBoard documentation spells the status key this way.
  r.respond("{\"credentialsType\":\"X509_CERTIFICATE\",\"credentialsValue\":\"MIIB\","
            "\"provisionDeviceStatus\":\"SUCCESS\"}");
  TEST_ASSERT_EQUAL(Result::Ok, done.r);
  TEST_ASSERT_EQUAL_STRING("", r.t.lastUser.c_str());
  TEST_ASSERT_TRUE(r.t.lastOpts.tls == &tls);
  TEST_ASSERT_TRUE(r.prov.credentials().kind == Credentials::Kind::X509);
}

static void test_refused_or_invalid_response_stops_the_client() {
  {
    ProvRig r;
    r.request();
    r.respond("{\"errorMsg\":\"Provision data was not found!\",\"status\":\"NOT_FOUND\"}");
    TEST_ASSERT_EQUAL(1, done.n);
    TEST_ASSERT_EQUAL(Result::Rejected, done.r);
    TEST_ASSERT_EQUAL_STRING("Provision data was not found!", done.error.c_str());
    TEST_ASSERT_EQUAL(State::Idle, r.c.state());
    TEST_ASSERT_TRUE(r.store.data.empty());
    // A second response after the end is ignored.
    r.respond(kOkToken);
    TEST_ASSERT_EQUAL(1, done.n);
  }
  const char* invalid[] = {
      "{\"credentialsType\":\"LWM2M_CREDENTIALS\",\"status\":\"SUCCESS\"}",
      "{\"credentialsType\":\"ACCESS_TOKEN\",\"credentialsValue\":\"\",\"status\":\"SUCCESS\"}",
      "{\"credentialsType\":\"MQTT_BASIC\",\"credentialsValue\":{\"password\":\"p\"},\"status\":\"SUCCESS\"}",
      "not json",
  };
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    ProvRig r;
    r.request();
    r.respond(invalid[i]);
    TEST_ASSERT_EQUAL_MESSAGE(Result::Rejected, done.r, invalid[i]);
    TEST_ASSERT_EQUAL(State::Idle, r.c.state());
  }
}

static void test_times_out() {
  ProvRig r;
  r.request();
  r.clock.advance(VL_PROVISION_TIMEOUT_MS);
  r.c.loop();
  TEST_ASSERT_EQUAL(1, done.n);
  TEST_ASSERT_EQUAL(Result::Timeout, done.r);
  TEST_ASSERT_EQUAL(State::Idle, r.c.state());
  // It can be started again.
  r.t.subscribed.clear();
  r.request();
}

static void test_request_sent_again_after_a_reconnect() {
  ProvRig r;
  r.request();
  r.t.close();
  r.clock.advance(2000);
  r.c.loop();
  TEST_ASSERT_EQUAL_STRING("provision", r.t.lastUser.c_str());
  r.t.connack(0);
  r.t.published.clear();
  r.c.loop();
  TEST_ASSERT_EQUAL_UINT(1, r.t.published.size());
  TEST_ASSERT_EQUAL_STRING("/provision/request", r.t.published[0].topic.c_str());
  r.respond(kOkToken);
  TEST_ASSERT_EQUAL(Result::Ok, done.r);
}

static void test_setup_errors_and_forget() {
  ProvRig r;
  Provisioning bare(&r.store);
  TEST_ASSERT_EQUAL(Result::Rejected, bare.start(r.c, r.cfg));  // no setDevice()
  TEST_ASSERT_EQUAL(Result::Rejected, bare.setDevice("x", "", "S"));
  std::string longKey(VL_PROVISION_KEY_SIZE, 'k');
  TEST_ASSERT_EQUAL(Result::TooLarge, bare.setDevice("x", longKey.c_str(), "S"));
  // Without a name the server picks one.
  TEST_ASSERT_EQUAL(Result::Ok, r.prov.setDevice(nullptr, "KEY", "SECRET"));
  r.store.data["vl_cred"] = blob(1, "", "SAVED", "");
  TEST_ASSERT_EQUAL(Result::Ok, r.prov.forget());
  TEST_ASSERT_TRUE(r.store.data.empty());
  std::string req = r.request();
  TEST_ASSERT_EQUAL_STRING("{\"provisionDeviceKey\":\"KEY\",\"provisionDeviceSecret\":\"SECRET\"}", req.c_str());
}

static void test_unsaved_credentials_still_connect() {
  ProvRig r;
  r.store.failSet = true;
  r.request();
  r.respond(kOkToken);
  TEST_ASSERT_EQUAL(Result::Ok, done.r);
  TEST_ASSERT_EQUAL_STRING("NEWTOKEN", r.t.lastUser.c_str());
  TEST_ASSERT_TRUE(r.store.data.empty());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_saved_credentials_skip_provisioning);
  RUN_TEST(test_provisions_with_server_generated_token);
  RUN_TEST(test_device_chosen_credentials);
  RUN_TEST(test_x509);
  RUN_TEST(test_refused_or_invalid_response_stops_the_client);
  RUN_TEST(test_times_out);
  RUN_TEST(test_request_sent_again_after_a_reconnect);
  RUN_TEST(test_setup_errors_and_forget);
  RUN_TEST(test_unsaved_credentials_still_connect);
  return UNITY_END();
}
```

- [ ] **Step 3: Run it to verify it fails.** `pio test -e native -f test_provision`. Expected: compile error,
  `vl/api/Provision.h: No such file or directory`.

- [ ] **Step 4: Client.** Apply (this includes Task 2 Step 6):

```diff
--- a/src/vl/Client.h
+++ b/src/vl/Client.h
@@ -4,6 +4,7 @@
 #include <stdint.h>
 
 #include "vl/Config.h"
+#include "vl/Credentials.h"
 #include "vl/IApiModule.h"
 #include "vl/Payload.h"
 #include "vl/Result.h"
@@ -19,37 +20,6 @@
 
 namespace vl {
 
-struct Credentials {
-  enum class Kind : uint8_t { None, AccessToken, MqttBasic, X509 };
-  Kind kind = Kind::None;
-  const char* clientId = nullptr;
-  const char* username = nullptr;
-  const char* password = nullptr;
-};
-
-inline Credentials AccessToken(const char* token) {
-  Credentials c;
-  c.kind = Credentials::Kind::AccessToken;
-  c.username = token;
-  return c;
-}
-
-inline Credentials MqttBasic(const char* clientId, const char* username, const char* password) {
-  Credentials c;
-  c.kind = Credentials::Kind::MqttBasic;
-  c.clientId = clientId;
-  c.username = username;
-  c.password = password;
-  return c;
-}
-
-// Device identified by its TLS client certificate (set Config::tls).
-inline Credentials X509() {
-  Credentials c;
-  c.kind = Credentials::Kind::X509;
-  return c;
-}
-
 // Plain struct (C++11). Strings are copied by begin().
 struct Config {
   const char* host = nullptr;
@@ -127,7 +97,8 @@
 #endif
 
   State state() const { return session_.state(); }
-  bool online() const { return session_.online(); }
+  // False during a provisioning session, which only its module may use.
+  bool online() const { return session_.online() && !exclusive_; }
 
   // Periodic task on the client task, timed on uptime. Thread-safe.
   Result every(uint32_t intervalMs, Scheduler::Task fn, void* ctx);
@@ -161,6 +132,16 @@
   // Guards the outbox and module tables: hold it only around table access,
   // never across network I/O or a callback.
   ILock& lock() { return *lock_; }
+  // Exclusive session (vl::Provisioning): like begin(), but only m's topics
+  // are subscribed, only m sees Online, online() stays false and nothing
+  // leaves the outbox. begin() or setCredentials() returns to the normal
+  // session. m must be attached; NotReady otherwise.
+  Result beginExclusive(const Config& cfg, IApiModule& m);
+  typedef void (*PayloadFn)(JsonWriter& w, void* ctx);
+  // Exclusive session only, on the client task: fn writes the payload into the
+  // transmit buffer and it is published at once, bypassing the outbox.
+  // NotReady when not connected, TooLarge if it did not fit.
+  Result publishNow(OutTopic topic, uint32_t arg, PayloadFn fn, void* ctx);
   void setDeviceNames(DeviceNameFn fn, void* ctx) {
     deviceName_ = fn;
     deviceNameCtx_ = ctx;
@@ -188,6 +169,7 @@
   static void thunkMessage(const char* topic, const uint8_t* p, size_t n, void* ctx);
   static void thunkClosed(void* ctx);
 
+  Result beginSession(const Config& cfg, IApiModule* exclusive);
   void drain(uint64_t nowMs);
   void syncClock(uint64_t nowMs);
   Result copyCredentials(const Credentials& auth);
@@ -213,11 +195,12 @@
   void* authCtx_ = nullptr;
   DeviceNameFn deviceName_ = nullptr;
   void* deviceNameCtx_ = nullptr;
+  IApiModule* exclusive_ = nullptr;
 
   char host_[64] = {};
   char clientId_[24] = {};
-  char username_[96] = {};
-  char password_[64] = {};
+  char username_[VL_USERNAME_SIZE] = {};
+  char password_[VL_PASSWORD_SIZE] = {};
   char topicBuf_[96] = {};
   char txBuf_[VL_TX_BUFFER_SIZE];
 
```

```diff
--- a/src/vl/Client.cpp
+++ b/src/vl/Client.cpp
@@ -64,6 +64,7 @@
 }
 
 void Client::detach(IApiModule& m) {
+  if (exclusive_ == &m) exclusive_ = nullptr;
   {
     LockGuard g(*lock_);
     pending_.removeOwner(&m);
@@ -96,7 +97,16 @@
   return Result::Ok;
 }
 
-Result Client::begin(const Config& cfg) {
+Result Client::begin(const Config& cfg) { return beginSession(cfg, nullptr); }
+
+Result Client::beginExclusive(const Config& cfg, IApiModule& m) {
+  bool attached = false;
+  for (size_t i = 0; i < router_.moduleCount(); ++i) attached |= &router_.module(i) == &m;
+  if (!attached) return Result::NotReady;
+  return beginSession(cfg, &m);
+}
+
+Result Client::beginSession(const Config& cfg, IApiModule* exclusive) {
   if (!cfg.host || !cfg.host[0] || cfg.maxInFlight == 0) return Result::Rejected;
   if (cfg.auth.kind == Credentials::Kind::None) return Result::Rejected;
   if (cfg.auth.kind == Credentials::Kind::X509 && !cfg.tls) return Result::Rejected;
@@ -115,6 +125,8 @@
     LockGuard g(*lock_);
     outbox_.setMaxPayload(limit > kEnvelopeOverhead ? limit - kEnvelopeOverhead : 0);
   }
+  exclusive_ = exclusive;
+  router_.setSolo(exclusive);
   bucket_.configure(cfg.ratePerSec, cfg.rateBurst);
   session_.setTiming(cfg.timing);
   session_.seed(platformRandom());
@@ -137,6 +149,8 @@
   if (!begun_) return Result::NotReady;
   Result r = copyCredentials(auth);
   if (r != Result::Ok) return r;
+  exclusive_ = nullptr;
+  router_.setSolo(nullptr);
   ConnectOptions o;
   o.host = host_;
   o.port = cfg_.port;
@@ -178,7 +192,7 @@
   uint64_t now = clock_->uptimeMs();
   session_.tick(now);
   syncClock(now);
-  if (session_.online()) drain(now);
+  if (online()) drain(now);
 
   Correlator::Callback cb;
   void* ctx;
@@ -367,6 +381,18 @@
   return pending_.take(id, cb, ctx);
 }
 
+Result Client::publishNow(OutTopic topic, uint32_t arg, PayloadFn fn, void* ctx) {
+  if (!exclusive_ || !session_.online()) return Result::NotReady;
+  // The drain, the only other user of these buffers, is off in this session.
+  JsonWriter w(txBuf_, sizeof(txBuf_));
+  fn(w, ctx);
+  if (w.overflowed()) return Result::TooLarge;
+  if (formatTopic(topic, arg, 0, topicBuf_, sizeof(topicBuf_)) == 0) return Result::Rejected;
+  uint16_t packetId = 0;
+  bool sent = transport_.publish(topicBuf_, reinterpret_cast<const uint8_t*>(txBuf_), w.size(), 1, &packetId);
+  return sent ? Result::Ok : Result::NotReady;
+}
+
 Stats Client::stats() {
   LockGuard g(*lock_);
   Stats s;
@@ -384,7 +410,11 @@
 void Client::onSessionState(State from, State to) {
   (void)from;
   if (to == State::Online) {
-    for (size_t i = 0; i < router_.moduleCount(); ++i) router_.module(i).onOnline();
+    if (exclusive_) {
+      exclusive_->onOnline();
+    } else {
+      for (size_t i = 0; i < router_.moduleCount(); ++i) router_.module(i).onOnline();
+    }
   }
   if (stateCb_) stateCb_(to, stateCtx_);
 }
```

- [ ] **Step 5: Create `src/vl/pal/KeyValueStore.h`**

```cpp
#pragma once

#include <stddef.h>

#include "vl/Result.h"

namespace vl {

// Small durable blobs: credentials, and later OTA progress. Keys are at most
// 15 characters (the ESP32 NVS limit). Used on the client task, or before
// begin(); implementations need not be thread-safe.
class IKeyValueStore {
 public:
  virtual ~IKeyValueStore() {}
  // Copies the value of key into buf and its size into len. Rejected if the
  // key is missing, TooLarge if the value is longer than cap.
  virtual Result get(const char* key, void* buf, size_t cap, size_t& len) = 0;
  // Returns once the value is durable.
  virtual Result set(const char* key, const void* value, size_t len) = 0;
  // Ok also when the key was missing.
  virtual Result erase(const char* key) = 0;
};

}  // namespace vl
```

- [ ] **Step 6: Create `src/vl/api/Provision.h`**

```cpp
#pragma once

#include "vl/Config.h"

#if VL_ENABLE_PROVISION

#include <stddef.h>
#include <stdint.h>

#include "vl/Client.h"
#include "vl/Credentials.h"
#include "vl/IApiModule.h"
#include "vl/Result.h"
#include "vl/core/JsonReader.h"
#include "vl/pal/KeyValueStore.h"

namespace vl {

// Provisioning ended. On Ok the device now runs its normal session with
// Provisioning::credentials(). Otherwise the client is stopped; error is the
// server's message or a short reason, valid during the call.
typedef void (*ProvisionDoneCb)(Result r, const char* error, void* ctx);

// ThingsBoard device provisioning. start() replaces client.begin(): with
// saved credentials the normal session starts at once; without them a short
// provisioning session (username "provision", only /provision/response
// subscribed) sends the request, saves the credentials it gets back and then
// starts the normal session with them. Other modules and the outbox wait for
// the normal session. Runs on the client task.
class Provisioning : public ApiModule {
 public:
  // store null: nothing is saved, so every boot provisions again.
  explicit Provisioning(IKeyValueStore* store = nullptr) : store_(store) {}

  // The device profile's provision key and secret; name null lets the server
  // name the device. Copied.
  Result setDevice(const char* name, const char* key, const char* secret);
  // Credentials the device brings: AccessToken(token) or MqttBasic(...),
  // copied, or X509() with setCertificate(). Default: the server generates an
  // access token.
  Result setCredentials(const Credentials& c);
  // The device certificate for X509(), sent as PEM. Not copied: it must
  // outlive provisioning. Config::tls must present the same certificate.
  void setCertificate(const char* certPem) { certPem_ = certPem; }
  void onDone(ProvisionDoneCb cb, void* ctx) {
    doneCb_ = cb;
    doneCtx_ = ctx;
  }

  // Starts the client with saved credentials, or provisions first. Call
  // instead of client.begin(). Rejected without setDevice(), or for X509
  // without a certificate and Config::tls.
  Result start(Client& client, const Config& cfg);
  // Erases the saved credentials, e.g. on AuthFailed before start() again.
  Result forget();
  // Saved or provisioned credentials; valid while this object lives.
  const Credentials& credentials() const { return creds_; }

  void topics(TopicSink& sink) override { sink.add(InTopic::ProvisionResponse); }
  bool handle(const Topic& topic, Bytes payload) override;
  void onOnline() override;
  void tick(uint64_t nowMs) override;

 private:
  Credentials view(Credentials::Kind kind) const;
  bool load();
  void save();
  void finish();
  bool takeCredentials(JsonValue response);
  static void writeRequest(JsonWriter& w, void* ctx);

  IKeyValueStore* store_;
  ProvisionDoneCb doneCb_ = nullptr;
  void* doneCtx_ = nullptr;
  const char* certPem_ = nullptr;

  char name_[VL_PROVISION_NAME_SIZE] = {};
  char key_[VL_PROVISION_KEY_SIZE] = {};
  char secret_[VL_PROVISION_KEY_SIZE] = {};
  Credentials::Kind requested_ = Credentials::Kind::None;
  // Requested credentials before the response, then the ones in use.
  Credentials creds_;
  char clientId_[24] = {};
  char username_[VL_USERNAME_SIZE] = {};
  char password_[VL_PASSWORD_SIZE] = {};
  char error_[64] = {};

  bool active_ = false;  // provisioning session running
  bool send_ = false;    // request still to be published on this session
  bool done_ = false;    // response handled; tick() finishes
  Result result_ = Result::Ok;
  uint64_t deadline_ = 0;
};

}  // namespace vl

#endif  // VL_ENABLE_PROVISION
```

- [ ] **Step 7: Create `src/vl/api/Provision.cpp`**

```cpp
#include "vl/api/Provision.h"

#if VL_ENABLE_PROVISION

#include <string.h>

#include "vl/Log.h"
#include "vl/core/Codec.h"
#include "vl/core/JsonReader.h"

namespace vl {

namespace {

const char kStoreKey[] = "vl_cred";
// 'V', format version, kind, then clientId, username, password, each NUL-terminated.
const uint8_t kBlobVersion = 1;
const size_t kBlobSize = 3 + 24 + VL_USERNAME_SIZE + VL_PASSWORD_SIZE;

bool copyStr(char* dst, size_t cap, const char* src) {
  size_t n = src ? strlen(src) : 0;
  if (n >= cap) return false;
  if (n) memcpy(dst, src, n);
  dst[n] = '\0';
  return true;
}

// Absent stays ""; present must be a string that fits.
bool copyOptional(JsonValue v, char* dst, size_t cap) {
  dst[0] = '\0';
  return !v.valid() || v.copyString(dst, cap);
}

const char* orNull(const char* s) { return s[0] ? s : nullptr; }

}  // namespace

Result Provisioning::setDevice(const char* name, const char* key, const char* secret) {
  if (!key || !key[0] || !secret || !secret[0]) return Result::Rejected;
  if (!copyStr(name_, sizeof(name_), name) || !copyStr(key_, sizeof(key_), key) ||
      !copyStr(secret_, sizeof(secret_), secret)) {
    return Result::TooLarge;
  }
  return Result::Ok;
}

Result Provisioning::setCredentials(const Credentials& c) {
  if (c.kind == Credentials::Kind::AccessToken && (!c.username || !c.username[0])) return Result::Rejected;
  if (c.kind == Credentials::Kind::MqttBasic && !c.username && !c.clientId) return Result::Rejected;
  if (!copyStr(clientId_, sizeof(clientId_), c.clientId) || !copyStr(username_, sizeof(username_), c.username) ||
      !copyStr(password_, sizeof(password_), c.password)) {
    return Result::TooLarge;
  }
  requested_ = c.kind;
  creds_ = view(c.kind);
  return Result::Ok;
}

Credentials Provisioning::view(Credentials::Kind kind) const {
  Credentials c;
  c.kind = kind;
  c.clientId = orNull(clientId_);
  c.username = orNull(username_);
  c.password = orNull(password_);
  return c;
}

Result Provisioning::start(Client& client, const Config& cfg) {
  if (store_ && load()) {
    VL_LOGI("provision: using saved credentials");
    Config c = cfg;
    c.auth = creds_;
    return client.begin(c);
  }
  if (!key_[0]) return Result::Rejected;
  if (requested_ == Credentials::Kind::X509 && (!certPem_ || !cfg.tls)) return Result::Rejected;
  creds_ = view(requested_);
  Result r = client.attach(*this);
  if (r != Result::Ok) return r;
  Config c = cfg;
  c.auth = AccessToken("provision");
  active_ = true;
  send_ = false;
  done_ = false;
  deadline_ = client.uptimeMs() + VL_PROVISION_TIMEOUT_MS;
  r = client.beginExclusive(c, *this);
  if (r != Result::Ok) {
    active_ = false;
    client.detach(*this);
  }
  return r;
}

Result Provisioning::forget() { return store_ ? store_->erase(kStoreKey) : Result::Ok; }

bool Provisioning::handle(const Topic& topic, Bytes payload) {
  if (topic.kind != InTopic::ProvisionResponse) return false;
  if (!active_ || done_) return true;
  JsonValue v = JsonValue::parse(payload);
  JsonValue status = v["status"];
  if (!status.valid()) status = v["provisionDeviceStatus"];  // spelling in older ThingsBoard docs
  done_ = true;
  if (!status.equals("SUCCESS")) {
    result_ = Result::Rejected;
    if (!v["errorMsg"].copyString(error_, sizeof(error_))) copyStr(error_, sizeof(error_), "provisioning refused");
    VL_LOGE("provision: refused: %s", error_);
    return true;
  }
  result_ = takeCredentials(v) ? Result::Ok : Result::Rejected;
  return true;
}

bool Provisioning::takeCredentials(JsonValue response) {
  JsonValue type = response["credentialsType"];
  JsonValue value = response["credentialsValue"];
  Credentials::Kind kind;
  bool ok;
  clientId_[0] = username_[0] = password_[0] = '\0';
  if (type.equals("ACCESS_TOKEN")) {
    kind = Credentials::Kind::AccessToken;
    ok = value.copyString(username_, sizeof(username_)) && username_[0];
  } else if (type.equals("MQTT_BASIC")) {
    kind = Credentials::Kind::MqttBasic;
    ok = copyOptional(value["clientId"], clientId_, sizeof(clientId_)) &&
         copyOptional(value["userName"], username_, sizeof(username_)) &&
         copyOptional(value["password"], password_, sizeof(password_)) && (clientId_[0] || username_[0]);
  } else if (type.equals("X509_CERTIFICATE")) {
    kind = Credentials::Kind::X509;  // the certificate in Config::tls is the credential
    ok = true;
  } else {
    copyStr(error_, sizeof(error_), "unsupported credentials type");
    return false;
  }
  if (!ok) {
    copyStr(error_, sizeof(error_), "invalid credentials");
    return false;
  }
  creds_ = view(kind);
  return true;
}

void Provisioning::onOnline() {
  if (active_) send_ = true;
}

void Provisioning::tick(uint64_t nowMs) {
  if (!active_ || !client_) return;
  if (!done_ && nowMs >= deadline_) {
    result_ = Result::Timeout;
    copyStr(error_, sizeof(error_), "no response");
    done_ = true;
  }
  if (done_) {
    finish();
    return;
  }
  if (!send_) return;
  Result r = client_->publishNow(OutTopic::ProvisionRequest, 0, writeRequest, this);
  if (r == Result::Ok) {
    send_ = false;
    VL_LOGI("provision: request sent");
  } else if (r != Result::NotReady) {
    result_ = r;
    copyStr(error_, sizeof(error_), "request too large");
    done_ = true;
  }
}

void Provisioning::finish() {
  active_ = false;
  Client* c = client_;
  Result r = result_;
  // The normal session must not subscribe to /provision/response. Detaching
  // here makes the client skip the next module's tick once; harmless.
  c->detach(*this);
  if (r == Result::Ok) {
    save();
    VL_LOGI("provision: done, starting the session");
    c->setCredentials(creds_);
  } else {
    c->end();
  }
  if (doneCb_) doneCb_(r, r == Result::Ok ? nullptr : error_, doneCtx_);
}

void Provisioning::writeRequest(JsonWriter& w, void* ctx) {
  Provisioning* p = static_cast<Provisioning*>(ctx);
  codec::provisionRequest(w, orNull(p->name_), p->key_, p->secret_, p->view(p->requested_), p->certPem_);
}

bool Provisioning::load() {
  uint8_t blob[kBlobSize];
  size_t n = 0;
  if (store_->get(kStoreKey, blob, sizeof(blob), n) != Result::Ok || n < 6) return false;
  if (blob[0] != 'V' || blob[1] != kBlobVersion) return false;
  uint8_t kind = blob[2];
  if (kind < static_cast<uint8_t>(Credentials::Kind::AccessToken) || kind > static_cast<uint8_t>(Credentials::Kind::X509)) {
    return false;
  }
  char* dst[3] = {clientId_, username_, password_};
  const size_t caps[3] = {sizeof(clientId_), sizeof(username_), sizeof(password_)};
  size_t len[3];
  // Validate everything before touching the buffers, which hold the requested
  // credentials until a load succeeds.
  const uint8_t* p = blob + 3;
  const uint8_t* end = blob + n;
  for (int i = 0; i < 3; ++i) {
    const void* nul = memchr(p, 0, static_cast<size_t>(end - p));
    if (!nul) return false;
    len[i] = static_cast<size_t>(static_cast<const uint8_t*>(nul) - p);
    if (len[i] >= caps[i]) return false;
    p += len[i] + 1;
  }
  if (kind == static_cast<uint8_t>(Credentials::Kind::AccessToken) && len[1] == 0) return false;
  p = blob + 3;
  for (int i = 0; i < 3; ++i) {
    memcpy(dst[i], p, len[i] + 1);
    p += len[i] + 1;
  }
  creds_ = view(static_cast<Credentials::Kind>(kind));
  return true;
}

void Provisioning::save() {
  if (!store_) return;
  uint8_t blob[kBlobSize];
  size_t n = 0;
  blob[n++] = 'V';
  blob[n++] = kBlobVersion;
  blob[n++] = static_cast<uint8_t>(creds_.kind);
  const char* parts[3] = {clientId_, username_, password_};
  for (int i = 0; i < 3; ++i) {
    size_t len = strlen(parts[i]) + 1;
    memcpy(blob + n, parts[i], len);
    n += len;
  }
  Result r = store_->set(kStoreKey, blob, n);
  // The session still starts; the next boot provisions again.
  if (r != Result::Ok) VL_LOGE("provision: credentials not saved: %s", toString(r));
}

}  // namespace vl

#endif  // VL_ENABLE_PROVISION
```

- [ ] **Step 8: Run the tests.** `pio test -e native -f test_provision`: 9 test cases, all PASSED. Then
  `pio test -e native`: 144 test cases, all PASSED.

- [ ] **Step 9: Portability.** `scripts/check_portability.sh`. Expected: every compiler and board passes; AVR skips the
  module (`VL_ENABLE_PROVISION` is 0 in the Lite profile).

---

### Task 4: ESP32 `NvsStore`, public header and component build

**Files:**
- Create: `src/vl/platform/esp32/EspNvs.h`, `src/vl/platform/esp32/EspNvs.cpp`
- Modify: `src/ViraLink.h`, `CMakeLists.txt`

Not unit-testable on the host; Task 5's example build compiles it for arduino-esp32 and ESP-IDF 5.2.

- [ ] **Step 1: Create `src/vl/platform/esp32/EspNvs.h`**

```cpp
#pragma once

#include "vl/Config.h"

#if VL_PLATFORM_ESP32

#include <stddef.h>
#include <stdint.h>

#include "vl/pal/KeyValueStore.h"

namespace vl {

// IKeyValueStore over ESP32 NVS, for Arduino and ESP-IDF. Values are blobs in
// one NVS namespace, committed on every write. Enable NVS encryption
// (CONFIG_NVS_ENCRYPTION) to keep credentials encrypted at rest. The NVS
// partition is initialized on first use if the application has not done it;
// a partition that needs erasing is reported, never erased here.
class NvsStore : public IKeyValueStore {
 public:
  // ns: NVS namespace, at most 15 characters; not copied (use a literal).
  explicit NvsStore(const char* ns = "vl") : ns_(ns) {}
  ~NvsStore() override;

  Result get(const char* key, void* buf, size_t cap, size_t& len) override;
  Result set(const char* key, const void* value, size_t len) override;
  Result erase(const char* key) override;

 private:
  bool open();

  const char* ns_;
  uint32_t handle_ = 0;  // nvs_handle_t
  bool open_ = false;
};

}  // namespace vl

#endif  // VL_PLATFORM_ESP32
```

- [ ] **Step 2: Create `src/vl/platform/esp32/EspNvs.cpp`**

```cpp
#include "vl/platform/esp32/EspNvs.h"

#if VL_PLATFORM_ESP32

#include "nvs.h"
#include "nvs_flash.h"
#include "vl/Log.h"

namespace vl {

NvsStore::~NvsStore() {
  if (open_) nvs_close(handle_);
}

bool NvsStore::open() {
  if (open_) return true;
  esp_err_t err = nvs_flash_init();  // ESP_OK when the application already did it
  if (err != ESP_OK) {
    VL_LOGE("nvs: init failed: %s (the partition may need nvs_flash_erase())", esp_err_to_name(err));
    return false;
  }
  nvs_handle_t h = 0;
  err = nvs_open(ns_, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    VL_LOGE("nvs: open %s: %s", ns_, esp_err_to_name(err));
    return false;
  }
  handle_ = h;
  open_ = true;
  return true;
}

Result NvsStore::get(const char* key, void* buf, size_t cap, size_t& len) {
  if (!open()) return Result::NotReady;
  size_t n = cap;
  esp_err_t err = nvs_get_blob(handle_, key, buf, &n);
  if (err == ESP_ERR_NVS_NOT_FOUND) return Result::Rejected;
  if (err == ESP_ERR_NVS_INVALID_LENGTH) return Result::TooLarge;
  if (err != ESP_OK) {
    VL_LOGW("nvs: get %s: %s", key, esp_err_to_name(err));
    return Result::Rejected;
  }
  len = n;
  return Result::Ok;
}

Result NvsStore::set(const char* key, const void* value, size_t len) {
  if (!open()) return Result::NotReady;
  esp_err_t err = nvs_set_blob(handle_, key, value, len);
  if (err == ESP_OK) err = nvs_commit(handle_);
  if (err != ESP_OK) {
    VL_LOGE("nvs: set %s: %s", key, esp_err_to_name(err));
    return err == ESP_ERR_NVS_NOT_ENOUGH_SPACE ? Result::NoMemory : Result::Rejected;
  }
  return Result::Ok;
}

Result NvsStore::erase(const char* key) {
  if (!open()) return Result::NotReady;
  esp_err_t err = nvs_erase_key(handle_, key);
  if (err == ESP_ERR_NVS_NOT_FOUND) return Result::Ok;
  if (err == ESP_OK) err = nvs_commit(handle_);
  if (err != ESP_OK) {
    VL_LOGE("nvs: erase %s: %s", key, esp_err_to_name(err));
    return Result::Rejected;
  }
  return Result::Ok;
}

}  // namespace vl

#endif  // VL_PLATFORM_ESP32
```

- [ ] **Step 3: Header and component.**

```diff
--- a/src/ViraLink.h
+++ b/src/ViraLink.h
@@ -10,11 +10,13 @@
 #include "vl/api/Attributes.h"
 #include "vl/api/Gateway.h"
 #include "vl/api/Ota.h"
+#include "vl/api/Provision.h"
 #include "vl/api/Rpc.h"
 #include "vl/core/JsonReader.h"
 #include "vl/core/JsonWriter.h"
 
 #if VL_PLATFORM_ESP32
 #include "vl/platform/esp32/EspMqtt.h"
+#include "vl/platform/esp32/EspNvs.h"
 #include "vl/platform/esp32/EspOta.h"
 #endif
```

```diff
--- a/CMakeLists.txt
+++ b/CMakeLists.txt
@@ -11,7 +11,7 @@
     SRC_DIRS "src/vl" "src/vl/api" "src/vl/core" "src/vl/pal" "src/vl/platform/esp32"
     INCLUDE_DIRS "src"
     REQUIRES mqtt esp_timer esp_ringbuf
-    PRIV_REQUIRES esp_system mbedtls app_update esp_http_client)
+    PRIV_REQUIRES esp_system mbedtls app_update esp_http_client nvs_flash)
   return()
 endif()
 
```

---

### Task 5: Example, portability sketch, docs and gate

**Files:**
- Create: `examples/provisioning/provisioning.ino`
- Modify: `test/portability/portability.ino`, `library.json`, `README.md`,
  `docs/superpowers/plans/2026-10-25-roadmap.md`

- [ ] **Step 1: Create `examples/provisioning/provisioning.ino`**

```cpp
// First boot with a device-profile provisioning key; every later boot uses
// the saved credentials.
//
// - In ViraLink Console, enable provisioning on a device profile and copy its
//   provision key and secret below.
// - On the first boot the device asks the server to create "vl-<MAC>" and
//   gets an access token back, which is saved in NVS.
// - If the server later rejects the saved token (device deleted or its
//   credentials changed), they are forgotten and the device provisions again.

#include <WiFi.h>
#include <ViraLink.h>
#include <time.h>

static const char* WIFI_SSID = "your-ssid";
static const char* WIFI_PASS = "your-password";
static const char* VL_HOST = "console.viralink.ir";
static const char* PROVISION_KEY = "YOUR_PROVISION_KEY";
static const char* PROVISION_SECRET = "YOUR_PROVISION_SECRET";

vl::EspMqtt mqtt;
vl::RingOutbox<8 * 1024> outbox;
vl::Client client(mqtt, outbox);
vl::NvsStore store;
vl::Provisioning prov(&store);

static void onProvisioned(vl::Result r, const char* error, void* /*ctx*/) {
  if (r == vl::Result::Ok) {
    Serial.println("provisioned; credentials saved");
  } else {
    Serial.printf("provisioning failed: %s (%s)\n", vl::toString(r), error ? error : "");
  }
}

static void onAuthFailed(void* /*ctx*/) {
  Serial.println("credentials rejected; provisioning again after a restart");
  prov.forget();
  ESP.restart();
}

static void sendUptime(void* /*ctx*/) { client.telemetry().add("uptime_s", millis() / 1000).send(); }

void setup() {
  Serial.begin(115200);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, "pool.ntp.org");

  char name[24];
  uint64_t mac = ESP.getEfuseMac();
  snprintf(name, sizeof(name), "vl-%04x%08lx", static_cast<unsigned>(mac >> 32), static_cast<unsigned long>(mac));
  prov.setDevice(name, PROVISION_KEY, PROVISION_SECRET);
  prov.onDone(onProvisioned, nullptr);

  client.onState([](vl::State s, void*) { Serial.printf("state: %s\n", vl::toString(s)); }, nullptr);
  client.onAuthFailed(onAuthFailed, nullptr);
  client.every(10000, sendUptime, nullptr);

  vl::Config cfg;
  cfg.host = VL_HOST;
  prov.start(client, cfg);  // instead of client.begin(cfg)
}

void loop() {
  client.loop();
  delay(5);
}
```

- [ ] **Step 2: Manifest, portability sketch and README.**

```diff
--- a/library.json
+++ b/library.json
@@ -38,6 +38,11 @@
       "name": "ota",
       "base": "examples/ota",
       "files": ["ota.ino"]
+    },
+    {
+      "name": "provisioning",
+      "base": "examples/provisioning",
+      "files": ["provisioning.ino"]
     }
   ]
 }
```

```diff
--- a/test/portability/portability.ino
+++ b/test/portability/portability.ino
@@ -34,6 +34,14 @@
 #if VL_ENABLE_GATEWAY
 vl::Gateway<2> gw;
 #endif
+#if VL_ENABLE_PROVISION
+vl::Provisioning prov;  // no store: provisions on every boot
+
+static void onProvisioned(vl::Result r, const char* error, void*) {
+  Serial.println(vl::toString(r));
+  if (error) Serial.println(error);
+}
+#endif
 #if VL_ENABLE_OTA
 vl::Ota ota;
 
@@ -95,7 +103,14 @@
   vl::Config cfg;
   cfg.host = "example.org";
   cfg.auth = vl::AccessToken("TOKEN");
+#if VL_ENABLE_PROVISION
+  prov.setDevice("portability", "KEY", "SECRET");
+  prov.setCredentials(vl::MqttBasic("cid", "user", "pass"));
+  prov.onDone(onProvisioned, nullptr);
+  prov.start(client, cfg);
+#else
   client.begin(cfg);
+#endif
   Serial.println(static_cast<long>(v["a"].asInt()));
 }
 
```

````diff
--- a/README.md
+++ b/README.md
@@ -42,7 +42,8 @@
 clock is set are restamped when it syncs, so offline data keeps its real time.
 
 Examples: `examples/basic_telemetry` (Arduino), `examples/rpc_and_attributes` (Arduino),
-`examples/gateway` (Arduino), `examples/ota` (Arduino), `examples/espidf_basic_telemetry` (ESP-IDF).
+`examples/gateway` (Arduino), `examples/ota` (Arduino), `examples/provisioning` (Arduino),
+`examples/espidf_basic_telemetry` (ESP-IDF).
 
 ### API modules
 
@@ -178,6 +179,36 @@
 - The download blocks `loop()` for at most one chunk write, or one HTTP read
   of `VL_OTA_HTTP_BLOCK` bytes (up to `VL_OTA_HTTP_TIMEOUT_MS` on a stalled link).
 
+### Provisioning
+
+A device can create itself on the server with a device profile's provision
+key and secret, then keep the credentials it gets back:
+
+```cpp
+vl::NvsStore store;                // ESP32 NVS; any vl::IKeyValueStore works
+vl::Provisioning prov(&store);
+
+prov.setDevice("Meter-7", "PROVISION_KEY", "PROVISION_SECRET");  // name null: the server picks
+prov.onDone([](vl::Result r, const char* error, void*) { /* Ok, or Rejected with the server's message */ }, nullptr);
+prov.start(client, cfg);           // instead of client.begin(cfg)
+```
+
+- With saved credentials `start()` begins the normal session at once.
+  Otherwise it runs a short provisioning session first: username
+  `provision`, only `/provision/response` subscribed (without waiting for a
+  SUBACK, which ThingsBoard 3.6-3.8 never send), one request, and on success
+  the credentials are saved and the normal session starts with them. Other
+  modules and queued telemetry wait for that session.
+- By default the server generates an access token. The device can bring its
+  own with `prov.setCredentials(vl::AccessToken(token))`,
+  `vl::MqttBasic(clientId, username, password)`, or `vl::X509()` plus
+  `prov.setCertificate(pem)` and a `Config::tls` that presents that certificate.
+- A refusal (for example a wrong key) or no answer within
+  `VL_PROVISION_TIMEOUT_MS` (30 s) stops the client and reports the error.
+- `prov.forget()` erases the saved credentials; call it from
+  `client.onAuthFailed` to provision again after the device was deleted.
+- On ESP32, enable NVS encryption to keep the credentials encrypted at rest.
+
 ### Claiming
 
 `client.claim("secret", 60000)` lets a user claim the device in ViraLink
````

- [ ] **Step 3: Gate.**

```sh
pio test -e native            # Expected: 144 test cases, all PASSED
scripts/check_portability.sh  # Expected: 4 compilers and 3 boards, all SUCCESS
scripts/build_examples.sh     # Expected: exit 0; provisioning.ino and the ESP-IDF example build for esp32dev
```

- [ ] **Step 4: Roadmap.** In `docs/superpowers/plans/2026-10-25-roadmap.md`, tick
  `Provisioning session: access token, MQTT basic, X.509; IKeyValueStore` and note the sub-plan
  (`plan: 2026-10-26-phase5a-provisioning.md`) next to it.
