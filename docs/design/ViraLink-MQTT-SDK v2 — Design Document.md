# ViraLink-MQTT-SDK v2 — Design Document

Sep 28, 2026 · @Amir

## Summary

ViraLink-MQTT-SDK v2 is a new embedded library that combines the operational strengths of ViraLink-MQTT-Client 1.0.2 with the full ThingsBoard device API. From ViraLink it keeps the offline queue, network failover, gateway mode and health attributes. From the ThingsBoard Arduino SDK it takes provisioning, two-way RPC, attribute requests and OTA. It is wire-compatible with the ThingsBoard CE MQTT device and gateway APIs, so it works against ViraLink Console and any ThingsBoard server. It is a new project in its own repository, `ViraLink-MQTT-SDK`, written from scratch rather than patched, because the 1.0.2 defects sit in the queue, session and OTA cores. ViraLink-MQTT-Client 1.0.2 stays available, unchanged, for existing products.

### Goals

- Cover the whole ThingsBoard MQTT device API and gateway API: telemetry, attributes, attribute requests, shared-attribute updates, server and client RPC, claiming, provisioning, and firmware and software OTA.
- No silent data loss. Delivery is at-least-once: a message leaves the queue only after the broker acknowledges it, flash persistence is optional, and any record dropped because the outbox is full is counted and reported.
- Stay transport-agnostic: WiFi, Ethernet (LAN8720), GSM/LTE through TinyGSM, and TLS with mTLS, including keys held in an ATECC608B.
- Build for ESP32 (Arduino and ESP-IDF), ESP8266 and STM32, plus a reduced AVR profile.
- Keep memory deterministic: no heap allocation on the core path after `begin()`, and every feature can be switched off at compile time.
- Be safe to use from multiple FreeRTOS tasks.

### Non-goals for v2.0

- CoAP, LwM2M or an HTTP-only transport. HTTP telemetry can follow in v2.x.
- Device-side provisioning UX such as BLE, SoftAP portals or mobile apps.
- Serving as a general-purpose MQTT client for brokers other than ThingsBoard or ViraLink.

## Background

The two libraries are strong in opposite places. [ViraLink-MQTT-Client 1.0.2](https://github.com/viralinkio/ViraLink-MQTT-Client) (Apache-2.0, ESP32/ESP8266) gets the field-operation concerns right but covers only part of the API. The [ThingsBoard Arduino SDK](https://thingsboard.io/docs/reference/arduino-client-sdk/) (MIT) covers the API but has no gateway support and no offline buffering.

### What v2 keeps from each

| Source | Carried into v2 |
| --- | --- |
| ViraLink 1.0.2 | Publish queue that survives network drops; `NetworkInterfacesController` priority failover across WiFi, Ethernet and GSM; gateway telemetry, attributes and RPC; periodic system attributes; 64-bit uptime; running the MQTT loop on a separate core; TinyGSM + SSLClient examples |
| ThingsBoard SDK | One pluggable module per API (`IAPI_Implementation`), compiled only if used; MQTT client behind an interface (`IMQTT_Client`); provisioning; client-side RPC; RPC reply helper; attribute requests with callbacks; OTA over MQTT; native ESP-IDF build; compile-time sizing |

### v1.0.2 defects v2 must not repeat

| Defect in 1.0.2 | Root cause | v2 rule |
| --- | --- | --- |
| One failing message stalls the queue forever | `peek()` returns a copy, so the retry counter never grows | Size checked when a message is enqueued; retries tracked in the stored record; stuck messages go to a dead-letter counter |
| "No data loss" is not guaranteed | QoS0 publish; RAM-only `std::vector<String>` queue | QoS1 and dequeue on PUBACK; fixed ring buffer; optional flash persistence |
| Gateway connect, disconnect and claim payloads are rejected | Raw name or stringified JSON sent instead of an object | Gateway payloads built by one tested encoder, with golden-file tests |
| Gateway subscriptions lost after a reconnect | Only device topics are re-subscribed | A subscription registry is replayed every time the client comes online |
| OTA fails with ThingsBoard defaults and can hang | MD5 only; no chunk timeout; partition left open | SHA-256 by default plus the other TB algorithms; per-chunk timeout, retry and abort |
| No recovery from an invalid token | Marked `todo` in `MqttController.tpp` | Auth failure becomes its own state with an event and slower retry |
| Registry installs fail to compile | `library.json` pins ArduinoJson 6 while the code uses the v7 API | CI builds from the published manifest and lockfile |
| Examples in the README and CI are missing | Example folders deleted, references kept | CI builds every example on every target |
| Uptime can miss a `millis()` wrap | Wrap counted only on core 1 | `esp_timer` on ESP32; a single wrap-safe clock elsewhere |
| Latent undefined behavior | `delete` of an uninitialized pointer; uninitialized members | Members initialized at declaration; `-Wall -Werror` plus sanitizers on the host build |

The ThingsBoard SDK has open field issues worth designing against. One is an ESP8266 client that never reconnects after an outage until a reboot ([#271](https://github.com/thingsboard/thingsboard-client-sdk/issues)). Another is a request to receive large shared attributes in chunks rather than one message ([#270](https://github.com/thingsboard/thingsboard-client-sdk/issues)).

## Feature scope

v2.0 ships every row below except those marked v2.x. In the ThingsBoard SDK column, "—" means the feature is not in its published feature list.

| Feature | ViraLink 1.0.2 | TB SDK | v2 |
| --- | --- | --- | --- |
| Telemetry and attribute publish | ✓ | ✓ | 2.0, with device timestamps `{ts, values}` |
| Scheduled telemetry | — | — | 2.0, non-blocking scheduler |
| Attribute request (client and shared) | ✓, callbacks never expire | ✓ | 2.0, with timeout |
| Shared-attribute update subscription | Raw callback | ✓ | 2.0, per-key handlers, large payloads parsed through a filter |
| Attribute binding to variables | — | — | 2.0, shared (cloud to device) and client (device to cloud) |
| Server-side RPC | Receive only | ✓ | 2.0, method registry and automatic reply |
| Client-side RPC | — | ✓ | 2.0 |
| Device claiming | ✓ | ✓ | 2.0 |
| Device provisioning | — | ✓ | 2.0: access token, MQTT basic, X.509 |
| Firmware OTA over MQTT | MD5 only | ✓ | 2.0, all TB checksum algorithms, resume, rollback |
| Firmware OTA from a URL (`fw_url`) | — | — | 2.0, HTTPS with Range resume |
| Software OTA (`sw_*`) | — | — | v2.x, same engine |
| Gateway: connect, disconnect, telemetry, attributes, RPC | Partial, payload bugs | — | 2.0 |
| Gateway: attribute request, claim | Claim broken | — | 2.0 |
| Offline outbox (RAM) | Unbounded `vector` | — | 2.0, ring buffer with priority classes |
| Persistent outbox (flash) | — | — | 2.0, opt-in |
| QoS1 with dequeue on PUBACK | — | — | 2.0 |
| TLS and mTLS | Examples only | Via client | 2.0, TLS provider interface |
| ATECC608B key storage | — | — | 2.0 (ESP32) |
| Multi-network failover | ✓, no fail-back | — | 2.0, with fail-back |
| Health attributes | ✓ | — | 2.0, configurable keys |
| HTTP transport | — | Partial | v2.x |
| Native ESP-IDF | — | ✓ | 2.0 |
| STM32 | — | — | 2.0 |
| AVR | — | ✓ | 2.0, lite profile |

## Server compatibility

v2 targets ThingsBoard CE 3.6 through the `lts-4.4` branch with a single code path. Across all of these versions the MQTT device and gateway topics, the OTA attribute keys, the checksum algorithm list and the 64 KB default payload limit are identical. 4.4 has no release tag yet, so ViraLink builds pin a specific `lts-4.4` commit.

| Server behaviour | Versions | v2 rule |
| --- | --- | --- |
| The highest QoS granted is 1 | All | QoS1 for everything that must arrive; QoS2 is never requested |
| PUBACK is sent only after the transport service accepts the message | All | PUBACK is the delivery confirmation that frees an outbox record |
| An invalid payload closes the session on MQTT 3.1.1, unless the device profile option to PUBACK validation failures is on | All | Poison-record detection in the outbox; ViraLink device profiles turn that option on |
| More than 100 messages queued before the session is accepted closes it (`MQTT_MSG_QUEUE_SIZE_PER_DEVICE_LIMIT`) | All | Nothing is published before the CONNACK and every SUBACK have arrived |
| No SUBACK is sent for `/provision/response` | 3.6–3.8 | The provisioning session does not wait for that SUBACK |
| Client ID length is checked against the MQTT protocol version | 4.2+ | MQTT 3.1.1; client ID of 23 characters or fewer |
| The session can be closed when an RPC delivery times out (off by default; the default server RPC timeout is 10 s) | 3.9+ | Every RPC gets a reply, including unknown methods; replies go to the head of the outbox |
| Rate limits apply to the gateway API | 3.7+ | The outbox drains through a configurable token bucket |
| Payload limit of 64 KB (`NETTY_MAX_PAYLOAD_SIZE`); an OTA chunk larger than it is rejected | All | Largest publish and OTA chunk = the smaller of the transport buffer and the server limit |
| OTA checksums are computed with Guava's hash functions | All | Golden-value tests against a real server for every algorithm, especially CRC32 and Murmur3, whose hex byte order can differ from common C libraries |

Checked on 2026-09-28 in the ThingsBoard source at tags v3.6 to v4.3 and on the `lts-4.4` branch (`MqttTopics`, `MqttTransportHandler`, `AbstractGatewaySessionHandler`, `JsonConverter`, `BaseOtaPackageService`, `thingsboard.yml`), and in the [release notes](https://thingsboard.io/docs/releases/releases-table/v4-0-x/).

## Architecture

The library has five layers. Each layer calls only the one below it, and every edge between layers is a C++ interface, so a product can swap its transport or platform port without touching the core.

&#91;embedded content: v2 layers · calls go downward only\]

The core (Session, Outbox, Correlator, Router, Codec) holds every reliability guarantee. The layers above and below it can be replaced without weakening those guarantees.

**Interfaces between layers**

- `IApiModule` is implemented by each API module. It declares its topics, handles the messages routed to it, and reacts to the client coming online.
- `IMqttTransport` covers a non-blocking connect that reports the CONNACK return code, disconnect, publish with QoS and a PUBACK callback, subscribe with the SUBACK result, and loop. It has adapters for esp-mqtt and for an Arduino client that supports QoS1.
- `INetwork` exposes connect, a health check, a priority, and a factory for the underlying `Client`.
- `ITlsProvider` supplies the CA bundle, the client certificate, and a key handle, which can be a slot on a secure element.
- `IOutboxStore` provides append, peek, commit, and recovery after power loss.
- `IKeyValueStore`, `IFlashWriter`, `IClock`, `ILock` and `ILogger` are the platform abstraction layer (PAL).

## Subsystem design

### Session

One state machine owns the connection. It fixes the “no reconnect after an invalid token” bug by treating an auth failure as its own state.

&#91;embedded content: session state machine · 6 states\]

- **Protocol.** MQTT 3.1.1 with clean session on. The client ID is 23 characters or fewer, which every compliant broker must accept; by default it is derived from the MAC address. Keepalive defaults to 60 s, and a missed PINGRESP counts as link lost.
- **Backoff.** Grows exponentially from 1 s to a 60 s cap, with ±20 % jitter so a fleet does not reconnect in lockstep after a broker restart.
- **CONNACK codes.**
  - 2 (identifier rejected), 4 (bad credentials) and 5 (not authorized) lead to AUTH\_FAILED and raise an `AuthFailed` event, so the application can load new credentials or start provisioning.
  - 3 (server unavailable, which ThingsBoard also returns for connection rate limits) leads to BACKOFF with a minimum delay of 30 s.
- **Subscriptions.** On every entry into ONLINE the session replays the subscription registry: device topics, gateway topics, and gateway connect events for sub-devices marked connected. With clean session on, this replay is the only source of truth. Nothing is published until the CONNACK and every SUBACK have arrived, because the server closes a session that queues more than 100 messages before it is accepted.
- **Provisioning session.** Provisioning uses its own short session: connect with username `provision`, subscribe to `/provision/response` (without waiting for a SUBACK, which 3.6–3.8 never send), publish the request, store the credentials, disconnect, then start the normal session.
- **Delivery semantics: at-least-once.** A record that has not been acknowledged when the link drops is published again after reconnect, so the server can receive it twice. Timestamped telemetry is safe to repeat, because ThingsBoard stores one value per key and timestamp. Records without a timestamp and attribute writes may repeat; this is documented rather than hidden.

### Outbox

The outbox is a fixed-size byte ring. Defaults are 16 KB on ESP32, 8 KB on ESP8266 and STM32, and 1 KB on AVR. A record is `{len, seq, class, topic id, retry, ts, payload}`. Topics are stored as a small enum plus an optional sub-device index, not as `String`.

- **Classes decide only what may be dropped.** *Control* records (RPC replies, `fw_state`, claims, gateway connect and disconnect, provisioning) are never dropped. *Data* records (telemetry and attributes) are dropped oldest-first when the ring is full. 10 % of the ring is reserved for control; if even that reserve is full, a control enqueue returns `Result::QueueFull`. The drop counter is reported in the health attributes.
- **Ordering.** The outbox is one global FIFO, so attributes, telemetry and gateway connect or disconnect events leave in the order the application produced them. The only exception is RPC replies, which go to the head of the queue because the server times out an RPC after 10 s by default.
- **Delivery.** Records are published at QoS1 and freed only on PUBACK. Up to 4 records (configurable) are in flight at once, and TCP preserves their order.
- **Poison records.** On MQTT 3.1.1 ThingsBoard closes the session when a payload is invalid. A record gets a strike when the session closes while it is the oldest unacknowledged record and it was sent less than 5 s earlier. After 3 strikes it moves to a dead-letter counter and is logged. Ordinary outages add no strikes, so a bad link never causes data to be discarded. A payload larger than the largest allowed publish is rejected at enqueue with `Result::TooLarge`.
- **Drain rate.** Each `loop()` sends up to 8 records or runs up to 20 ms, and all sending passes through a token bucket (default 10 messages/s, configurable to match tenant-profile and gateway rate limits). Consecutive telemetry records for the same device are coalesced into one array publish, `[{"ts":…,"values":{…}}, …]`, up to the buffer size, so a backlog clears in fewer messages.
- **Persistence (opt-in).** An `IOutboxStore` backend (LittleFS segment files on ESP32, SPI flash or FRAM on STM32) appends records with CRC16 and sequence numbers. On boot, recovery scans to the last valid record and discards a torn tail.
  - Data records are flushed every 5 s or every 16 records; control records are written through immediately. The flush interval bounds how much data a power cut can lose.
  - The acknowledged position is saved on the same schedule, so after a reboot up to one flush window of already-delivered records may be sent again, which the at-least-once rule allows.
- **Timestamps.** Each record is stamped at enqueue with a boot ID and monotonic uptime. Once wall-clock time is known, the stamp is converted to UTC and sent as `{"ts":…,"values":{…}}`. Records from the current boot are converted retroactively when the clock first syncs, so data produced offline right after power-up still lands at the right time. Records from an earlier boot that never had UTC are sent without `ts`, and the server stamps them on arrival.

### Correlator and router

- The **correlator** is a single fixed table of pending requests, each with an id, a deadline and a callback. It is shared by attribute requests, client RPC, provisioning and OTA chunks. A request that times out fires its callback with `Result::Timeout`, so pending entries cannot pile up as they do in 1.0.2.
- The **router** matches incoming topics against a prefix table built from what the modules registered. Binary topics such as OTA chunks go straight to their module with no JSON parse and no copy onto a stack array sized by the payload.

### API modules

- **Telemetry and attributes.**
  - A builder API writes into a caller-sized document, with an optional `ts`.
  - Attribute requests send `{"clientKeys":"a,b","sharedKeys":"c"}` and parse the response `{"client":{…},"shared":{…}}`.
  - Shared-attribute updates are dispatched per key list, and that includes deletion notices of the form `{"deleted":[…]}`. Large payloads are parsed through an ArduinoJson filter so the device only allocates for keys it uses.
- **RPC.**
  - Server-side RPC uses a fixed-capacity method registry. Two-way methods get a reply object that publishes to `v1/devices/me/rpc/response/{id}` automatically, and an unknown method replies with an error, so the server never waits out its timeout.
  - Client-side RPC publishes to `v1/devices/me/rpc/request/{id}` and delivers the response to a callback, with a timeout shorter than the server's 60 s default.
- **Provisioning and claiming.** Provisioning uses the dedicated session described under Session and supports access-token, MQTT-basic and X.509 credentials. The returned credentials are saved through `IKeyValueStore`. Claiming works for both devices and gateways.
- **Gateway.** `vl::Gateway` keeps a sub-device table of name, type and connected flag. Payloads follow the ThingsBoard gateway API exactly:
  - connect and disconnect: `{"device":…,"type":…}`
  - telemetry: `{"Dev":[{"ts":…,"values":{…}}]}`
  - attributes: `{"Dev":{…}}`
  - attribute request: `{"id":…,"device":…,"client":true,"keys":[…]}`
  - RPC reply: `{"device":…,"id":…,"data":{…}}` on `v1/gateway/rpc`
  - claim: `{"Dev":{"secretKey":…,"durationMs":…}}`

  Sub-device messages share the outbox, tagged with the device index. Because the outbox keeps FIFO order, a connect event always precedes that device's data.
- **Health.** Keys are `vl_uptime`, `vl_heap_free`, `vl_heap_min`, `vl_reset_reason`, `vl_rssi`, `vl_iface`, `vl_queue_depth`, `vl_dropped`, `vl_dead_letter` and `vl_sdk`. The interval is configurable and each key can be switched off. The spaced key names from 1.0.2 are available behind a compatibility flag.
- **Topic and payload scope.** The library speaks the JSON `v1` topics. Device profiles must therefore use the JSON payload type and the default topic filters. Short `v2` topics and Protobuf payloads, both available in every server version from 3.6 on, are v2.x candidates for cellular devices.
- **Scheduler and scheduled telemetry.** `client.every(intervalMs, fn, ctx)` registers a periodic task in a fixed-capacity table (default 8, set at compile time).
  - Tasks run on the client task from `loop()`, never with a library lock held, and are timed on monotonic uptime.
  - Scheduling is drift-free: the next run is the previous due time plus the interval. After a stall, missed runs are skipped rather than fired in a burst.
  - Tasks keep running while offline; the telemetry they produce goes to the outbox like any other record.
- **Attribute binding.** A convenience layer on top of the attribute modules, with fixed capacity (default 8 bindings).
  - `shared.bind(key, &var)` binds a shared attribute to a variable of type `bool`, `int32_t`, `float`, or a `char` buffer with its size. When the session comes online, the module requests the current values, so bound variables are correct after a reboot. Every update or deletion notice rewrites the variable, and an optional change callback fires.
  - `attributes.report(key, &var, minIntervalMs)` binds a client attribute the other way. The scheduler checks the variable, and it is published only when it has changed and at least `minIntervalMs` has passed since the last report.
  - Bound variables are written only on the client task. A task that reads them from elsewhere must use atomic types or copy them under its own lock.

### OTA

- **Target discovery.** OTA implements the ThingsBoard v2 firmware protocol. On every boot the module reports `current_fw_title` and `current_fw_version`. On boot, and whenever the `fw_*` shared attributes change, it reads the target title, version, size, checksum and algorithm. If the target matches the running firmware, nothing happens.
- **State reporting.** `fw_state` is reported as DOWNLOADING, DOWNLOADED, VERIFIED, UPDATING and UPDATED, or FAILED with `fw_error`.
- **Checksums.** All algorithms ThingsBoard offers are supported: MD5, SHA256 (the UI default), SHA384, SHA512, CRC32, MURMUR3\_32 and MURMUR3\_128. The hash is computed while the image streams into flash, and the result is compared with golden values produced by a real server (see Server compatibility).
- **Chunks.** Chunks are requested on `v2/fw/request/{req}/chunk/{n}` and arrive on `v2/fw/response/{req}/chunk/{n}`. The chunk size is the smaller of the transport buffer and the server payload limit, minus protocol overhead. Each chunk has a 10 s timeout and 5 retries. A reconnect resumes at the current chunk with a new request ID. When retries run out, the module aborts the partition write and reports FAILED.
- **OTA from a URL.** If the shared attributes include `fw_url` (a ThingsBoard OTA package created with a URL instead of a file), the image is downloaded over HTTPS instead of MQTT chunks.
  - Checksum verification, `fw_state` reporting and activation are the same as for chunked downloads.
  - An interrupted download resumes with an HTTP `Range` request where the platform can reopen the partial image.
  - The download uses the `ITlsProvider` trust anchors, so the CA of the host serving the file must be included.
  - A package with no checksum is refused unless the build sets `VL_OTA_ALLOW_UNVERIFIED`.
  - The HTTP client comes from the platform layer: `esp_http_client` on ESP32, `HTTPClient` on ESP8266, the TinyGSM HTTP client on cellular.
- **Resume after reboot** is optional per platform. Progress (target checksum and last chunk) is saved in the KV store. On restart the already-written bytes are read back from flash to rebuild the hash before downloading continues. Platforms whose updater cannot reopen a partial image, such as ESP8266 `Updater` and Arduino `Update.h`, restart from chunk 0.
- **Activation and rollback.** The new boot partition is selected only after verification. UPDATED is reported once the new image has connected successfully; on ESP32 that is also when the image is marked valid, so a bad build rolls back automatically.
- **Software OTA** (`sw_*`) reuses the same engine and hands the bytes to an application callback (v2.x).

### Network manager

The manager ports `NetworkInterfacesController` onto `INetwork`. It keeps priority-ordered failover and adds **fail-back with hysteresis**: while the device runs on a lower-priority interface such as GSM, it probes higher-priority ones on a timer, and switches back only after one has passed its health check for 30 s in a row, so a flapping WiFi link does not bounce the session. An interface change rebinds the MQTT transport and sends the session to CONNECTING; unacknowledged records are resent under the at-least-once rule. Cellular goes through TinyGSM, and the modem's network time is used as a clock source.

### Security

- `ITlsProvider` supplies trust anchors, the client certificate and a key handle.
  - On ESP32, mbedTLS through `WiFiClientSecure` (`NetworkClientSecure` in arduino-esp32 3.x) covers both WiFi and LAN8720 Ethernet, since both run over lwIP.
  - BearSSL through SSLClient covers clients outside lwIP, such as TinyGSM modems and W5500 Ethernet.
  - With ATECC608B, the private key never leaves the chip.
- **Certificate validity needs a clock.** A TLS connection waits for a wall-clock source. Until one is available, validity is checked against a floor date compiled into the firmware, never skipped.
- **Pin the CA, not the server certificate.** ThingsBoard 4.2.2.2 and later reload server certificates without a restart, so pinning the leaf certificate would break devices at the next rotation.
- Credentials live only in the KV store (NVS encryption on ESP32), and the logger masks tokens.

### Time

- Uptime is 64-bit. It comes from `esp_timer` on ESP32 and from a single wrap-safe `millis()` extension on other targets, which removes the 1.0.2 core-1 workaround.
- Wall-clock time comes from a chain of `IClock` sources: SNTP, then cellular network time, then an RTC such as the DS1307. Records are stamped with uptime at enqueue and converted to UTC as described under Outbox.

## Public API sketch

Applications build one `vl::Client`, attach the modules they need, and call `loop()` or start a dedicated task. Every send returns a `vl::Result` instead of a bare `bool`.

```cpp
#include <ViraLink.h>

WiFiClientSecure net;
vl::ArduinoMqtt  mqtt(net);                 // QoS1-capable adapter
vl::RingOutbox<16 * 1024> outbox;           // RAM ring; add a store for persistence
vl::Client client(mqtt, outbox);

vl::Rpc<8>              rpc;                // up to 8 methods
vl::SharedAttributes<4> shared;             // up to 4 key subscriptions or bindings
vl::ClientAttributes<4> attributes;         // up to 4 reported variables
vl::Ota                 ota;                // MQTT chunks or fw_url over HTTPS
vl::Gateway<16>         gw;                 // up to 16 sub-devices

int32_t intervalMs = 10000;                 // bound to shared attribute "interval"
char    mode[16]   = "auto";                // bound to shared attribute "mode"
int32_t doorState  = 0;                     // reported as client attribute "door"

// Handlers are plain functions (or capture-less lambdas): no std::function, no heap.
static void onSetRelay(const vl::RpcRequest& req, vl::RpcReply& reply) {
  bool on = req.params()["on"] | false;     // ArduinoJson default-value syntax
  relay.set(on);
  reply.data()["on"] = relay.get();
  reply.send();                             // unanswered requests get an error reply
}

static void sendClimate(void* /*ctx*/) {    // runs on the client task
  client.telemetry().add("temp", sensor.temp()).add("rh", sensor.rh()).send();
  gw.device("Meter-12", "energy-meter").telemetry().add("kwh", meter.kwh()).send();
}

void setup() {
  client.use(rpc, shared, attributes, ota, gw);
  client.onState([](vl::State s, void*) { /* LEDs, logging */ }, nullptr);
  client.onAuthFailed([](void*) { /* load new token or start provisioning */ }, nullptr);

  rpc.on("setRelay", onSetRelay);
  shared.bind("interval", &intervalMs);
  shared.bind("mode", mode, sizeof(mode));
  attributes.report("door", &doorState, /*minIntervalMs*/ 1000);
  client.every(10000, sendClimate, nullptr);
  ota.begin("ViraGate", "2.3.0");

  vl::Config cfg;                           // plain struct: builds as C++11 on AVR
  cfg.host = "console.viralink.ir";
  cfg.port = 8883;
  cfg.auth = vl::AccessToken(nvs.token());
  client.begin(cfg);
  // ESP32: client.startTask(/*core*/ 0, /*prio*/ 5, /*stack*/ 6144);
}

void loop() {
  client.loop();                            // runs scheduled tasks; not needed after startTask()
  doorState = door.read();                  // same task as the client, so no lock needed
}
```

The two extension points a product is most likely to implement are sketched below.

```cpp
struct IMqttTransport {
  virtual ~IMqttTransport() = default;
  virtual void beginConnect(const ConnectOptions&) = 0;        // non-blocking; result via onConnack
  virtual void disconnect() = 0;
  virtual bool connected() const = 0;
  virtual bool publish(const char* topic, const uint8_t* p, size_t n,
                       uint8_t qos, uint16_t* packetId) = 0;     // false = not sent
  virtual void subscribe(const char* filter, uint8_t qos) = 0;  // result via onSuback
  virtual void loop() = 0;
  virtual size_t maxPacket() const = 0;

  // Callbacks run on the client task; ctx is passed back unchanged.
  virtual void onConnack(void (*cb)(uint8_t returnCode, void* ctx), void* ctx) = 0;
  virtual void onSuback(void (*cb)(const char* filter, bool granted, void* ctx), void* ctx) = 0;
  virtual void onPuback(void (*cb)(uint16_t packetId, void* ctx), void* ctx) = 0;
  virtual void onMessage(void (*cb)(const char* topic, const uint8_t* p, size_t n, void* ctx), void* ctx) = 0;
  virtual void onClosed(void (*cb)(void* ctx), void* ctx) = 0;
};

struct IApiModule {
  virtual ~IApiModule() = default;
  virtual void topics(TopicSink&) = 0;                        // subscriptions for the registry
  virtual bool handle(const Topic&, Span<const uint8_t>) = 0; // routed messages
  virtual void onOnline() {}
  virtual void tick(uint64_t /*nowMs*/) {}
};
```

`vl::Result` values are `Ok`, `Queued`, `TooLarge`, `QueueFull`, `NotReady`, `Timeout`, `Rejected` and `NoMemory`.

## Examples

Each example is a complete sketch covering one module, and CI builds every example on every target it lists.

| Example | Shows | Targets |
| --- | --- | --- |
| `basic_telemetry` | Connect, timestamped telemetry, scheduled telemetry with `client.every()` | All |
| `attribute_binding` | Shared attributes bound to variables; a client attribute reported on change | All |
| `rpc_control` | Relay control through server-side RPC with replies, plus a client-side RPC call | All |
| `gateway_mode` | Several sub-devices: connect, telemetry, attributes, RPC dispatch | ESP32, ESP8266, STM32 |
| `provisioning` | First boot with a provisioning key, credentials saved, normal session afterwards | ESP32, ESP8266, STM32 |
| `ota_mqtt` | Firmware update through MQTT chunks with `fw_state` reporting and rollback | ESP32, ESP8266, STM32 |
| `ota_url` | Firmware update from a package created with a URL (`fw_url`) over HTTPS | ESP32, ESP8266 |
| `offline_persistence` | Flash-backed outbox that survives reboots and replays with the original timestamps | ESP32, STM32 |
| `multi_network` | WiFi, Ethernet and GSM failover with fail-back | ESP32 |
| `secure_mtls` | TLS with CA pinning and mTLS, with the key held in an ATECC608B | ESP32 |
| `lite_avr` | Telemetry, attributes and server RPC within the Lite profile | ATmega2560 |
| `full_example` | All modules together in task mode | ESP32 |

## Memory, concurrency and platforms

Each target gets a build profile with a RAM budget. The budgets below are design targets to verify in M1. They exclude the outbox ring, TLS buffers and the application's own JSON documents.

| Profile | Targets | Library RAM target | Modules | Transport |
| --- | --- | --- | --- | --- |
| Full | ESP32 (Arduino, ESP-IDF) | ≤ 24 KB | All | esp-mqtt or Arduino adapter, TLS, ATECC608B |
| Standard | ESP8266, STM32F4/H7 | ≤ 12 KB | All except ATECC608B; OTA needs a dual-bank or external-flash layout on STM32 | Arduino adapter, BearSSL TLS |
| Lite | ATmega2560-class AVR | ≤ 2 KB | Telemetry, attributes, server RPC | Plain TCP only, no OTA, no persistence |

### Memory rules

- After `begin()`, nothing on the core path touches the heap. Capacities are template parameters, as in the ThingsBoard SDK.
- ArduinoJson v7 is supported through a custom pool allocator, so documents come from a fixed arena. The Lite profile uses a streaming JSON writer instead.
- No `String` appears in the core. Topics are enums, and payloads are spans into the outbox or the transport buffer.

### Concurrency model

- **One owner.** The client task, or whichever task calls `loop()`, is the only one that touches the socket, the session and the router.
- **Producers.** `send()`, `telemetry()` and `gw.device(...)` may be called from any task. They take a short FreeRTOS mutex with priority inheritance around the outbox append, and that mutex is never held during network I/O. This fixes 1.0.2, which held a binary semaphore across `publish()`. The API must not be called from an ISR; an ISR should set a flag or notify a task instead.
- **Callbacks.** Module callbacks (RPC handlers, attribute updates, OTA events) run on the client task and are never invoked while a library lock is held, so a handler can call `send()` without deadlocking. A handler that blocks stalls the session and must stay short.
- **Blocking and watchdogs.** Arduino network clients block in `connect()`, and a TLS handshake can take several seconds. In task mode the client task feeds the task watchdog around those calls. In `loop()` mode the documentation states the worst-case blocking time, and products with tight loops should use task mode.
- **Handlers without heap.** Callbacks are function pointers with a `void*` context rather than `std::function`, so registering a handler never allocates.

### Compile-time switches

`VL_ENABLE_OTA`, `VL_ENABLE_GATEWAY`, `VL_ENABLE_PROVISION`, `VL_ENABLE_PERSISTENCE`, `VL_ENABLE_HEALTH` and `VL_LOG_LEVEL` (0–4). A switched-off module adds no code.

## Testing, CI and release

A change is accepted only if host unit tests, the ThingsBoard integration suite and every example build all pass.

| Layer | What runs | Catches |
| --- | --- | --- |
| Host unit tests (PlatformIO `native`, sanitizers on) | Codec golden files; outbox power-cut fuzzing (truncate at a random byte, recover, compare); poison-record strikes; correlator timeouts; session state machine driven by a fake transport and a fake clock, including every CONNACK code | Queue stalls, torn records, leaks, wrong gateway payloads, reconnect loops |
| Integration (CI) | Host build of the client against ThingsBoard CE 3.6, 4.2, 4.3 and `lts-4.4` in Docker: broker restart, invalid token, 1 h outage with ordered replay and coalescing, duplicate delivery, invalid payload, provisioning on 3.6 (no SUBACK), OTA checksum golden values for all 7 algorithms, OTA with dropped and duplicated chunks, 50 gateway sub-devices under rate limits | Protocol drift and version differences |
| Hardware-in-the-loop (nightly) | ESP32 with WiFi and LAN8720, ESP8266, STM32 with SIM800 or EC200U; networks cut by relay | Reconnect after an outage without a reboot (ThingsBoard SDK issue #271 as an explicit test), failover and fail-back hysteresis, TLS on a cold boot with no clock, OTA across a power cut |
| Build matrix | Every example on every target and profile, built from the published `library.json` and a lockfile | Manifest and dependency drift, missing examples |

- **Versioning.** Semantic versioning, with v2.0.0 as the breaking release. Releases go to the PlatformIO Registry, the Arduino Library Manager and the ESP-IDF Component Registry.
- **License.** Apache-2.0, matching 1.0.2. Code adapted from the MIT-licensed ThingsBoard SDK keeps its copyright notice in a `NOTICE` file.
- **Docs.** Each module gets a README section and an example. A migration guide explains how to move from 1.0.2.

## Milestones, migration and decisions

Work proceeds in five phases. Each ends at a gate, and a phase starts only after the previous gate passes. The drawing shows order, not duration; dates are still to be set.

&#91;embedded content: roadmap · 5 phases, a gate after each (not to scale)\]

### Migrating from 1.0.2

- A `ViraLinkCompat.h` shim maps `MQTTController` (`sendTelemetry`, `sendAttributes`, `addToPublishQueue`, `requestAttributesJson`, `sendClaimRequest`) and `MQTTOTA` onto v2, so existing sketches compile after switching to the new library and including that header.
- Health-attribute keys change from names like `ESP Free Heap` to `vl_*`. Dashboards need updating, or set `VL_COMPAT_V1_KEYS`.
- Upgrading fielded devices over the air: 1.0.2 accepts only MD5, so the first v2 firmware package must be uploaded with an MD5 checksum. Every later update can use SHA-256.

### Decisions

| Question | Decision |
| --- | --- |
| Base | A new project written from scratch to this design. Nothing is carried over from the 1.0.2 code or from the internal Device-SDK; only the behaviours listed under Background are reimplemented. |
| Repository | A new `ViraLink-MQTT-SDK` repository. ViraLink-MQTT-Client stays at 1.0.2 and gets a README note pointing to the new library. |
| Server-side extras | None. The ViraLink fork uses the stock ThingsBoard CE device API, so there are no custom topics to support. |
| QoS1 transport | esp-mqtt on ESP32. lwmqtt (arduino-mqtt) on ESP8266 and STM32, with one QoS1 message in flight. |
| Persistence | LittleFS on a dedicated partition on ESP32; SPI NOR flash on STM32 boards; FRAM only where losing data at power-off is unacceptable. |
| STM32 bootloader | MCUboot. A custom dual-bank loader only if a board's flash is too small for it. |

- [ ] M0 benchmark: confirm that lwmqtt's one-in-flight QoS1 is fast enough on cellular. If it is not, write an in-house asynchronous client for ESP8266 and STM32.

## Review log

| Date | Change |
| --- | --- |
| 2026-09-28 | Reliability and standards review against the ThingsBoard source (3.6 to `lts-4.4`) and MQTT 3.1.1. Added a Server compatibility section. Delivery is now stated as at-least-once, with duplicates made safe by timestamps. Poison-record detection replaced a retry rule that could discard data during outages. Outbox ordering is global FIFO, with a control reserve and a rate-limited, coalescing drain. Session now handles every CONNACK code, the provisioning SUBACK gap on 3.6–3.8, and a 23-character client ID. OTA now reports `current_fw_*`, has a realistic resume rule, and uses checksum golden tests. TLS now handles a missing clock and pins the CA. Fail-back has hysteresis. The API sketch was fixed to build as C++11 and to use ArduinoJson syntax, and handlers are now heap-free. Rules added for ISRs, callbacks and watchdogs. |

## Sources

- [ThingsBoard Arduino Client SDK documentation](https://thingsboard.io/docs/reference/arduino-client-sdk/)
- [thingsboard-client-sdk issues](https://github.com/thingsboard/thingsboard-client-sdk/issues)
- [ViraLink-MQTT-Client 1.0.2 source](https://github.com/viralinkio/ViraLink-MQTT-Client), reviewed in full on 2026-09-28
- [ThingsBoard server source](https://github.com/thingsboard/thingsboard), tags v3.6 to v4.3 and the `lts-4.4` branch, reviewed on 2026-09-28
