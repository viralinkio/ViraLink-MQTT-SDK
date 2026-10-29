# Migrating from ViraLink-MQTT-Client 1.0.2

There are two routes. Both use this library in place of 1.0.2:

1. **Keep the sketch.** Include `ViraLinkCompat.h` instead of `viralink.h`.
   `MQTTController` and `MQTTOTA` keep their 1.0.2 signatures and run on the
   v2 client. This gets you the v2 outbox, session and OTA without touching
   application code.
2. **Port to the v2 API** (recommended for new work). It has no heap use after
   `begin()`, typed RPC and attribute handlers, and every module the shim does
   not expose. The [table below](#porting-to-the-v2-api) maps each 1.0.2 call.

## Route 1: the compatibility header

1. Replace the 1.0.2 library with this one, and keep ArduinoJson 7, which
   1.0.2 also needed. PubSubClient is no longer used.
2. Change the include:

   ```cpp
   #include <ViraLinkCompat.h>  // was: #include "viralink.h"
   ```

3. For a gateway, which 1.0.2 enabled with `subscribeToGatewayEvent()`,
   add `-DVL_COMPAT_GATEWAY=1` to the build flags. The gateway topics are
   then subscribed on every connect, and room for `VL_COMPAT_GW_DEVICES` (8)
   sub-devices is reserved.

[`examples/compat_v1`](../examples/compat_v1/compat_v1.ino) is a 1.0.2 sketch
built this way.

### What the sketch has to change

- **Callbacks are function pointers.** 1.0.2 took `std::function` for
  `registerCallback*()` and `requestAttributesJson()`. A lambda that captures
  nothing still converts; one that captures must become a function. The JSON
  callbacks take `(String topic, JsonDocument json)`, and the default
  callbacks keep their 1.0.2 signatures.
- **No shim** for `onSentMQTTMessageCallback()` and `MQTTMessage`, nor for
  1.0.2's helper headers: `NetworkController.h` / `NetworkInterface.h` (use
  `vl::Networks`), `Uptime.h` (`client.uptimeMs()`), `Button.h`,
  `PrintDBG.tpp` and `OTAUpdate.tpp`.
- `addToPublishQueue()` takes only the topics ThingsBoard defines: device
  telemetry, attributes, claim, attribute and RPC requests, RPC replies,
  and the gateway connect, disconnect, attribute request and RPC topics.
  It returns false for any other topic, and for gateway telemetry,
  attributes and claims. For those, call `sendTelemetry(json, deviceName)`,
  `sendAttributes(json, deviceName)` or
  `sendClaimRequest(key, ms, deviceName)`.

### What behaves differently underneath

| 1.0.2 | With the shim |
| --- | --- |
| A message left the queue once `publish()` returned | A record leaves the outbox only on its PUBACK and is resent after a reconnect (at-least-once) |
| After 5 failed publishes a message was dropped | Nothing is dropped for being slow. Only a record that keeps killing the session is set aside as a dead letter |
| `MQTTController(queueSize)` counted messages | The outbox holds `VL_COMPAT_OUTBOX_SIZE` bytes (16 KB). When it is full, the oldest telemetry is dropped and RPC replies and claims are kept |
| Telemetry had no timestamp, so the server used its arrival time | Telemetry is stamped when queued (UTC once the clock is set), so data sent after an outage keeps its time. A payload with its own `{"ts":…,"values":…}` keeps that timestamp |
| An invalid token left the client retrying forever (marked `todo` in the code) | Auth failure is a state of its own with a slower retry |
| `connect()` acted at once, on the calling task | `connect()` takes effect on the next `loop()`, on the task that runs it, so calling it from a network callback on another core is safe. Calling it again replaces the connection, e.g. after a WiFi to GSM switch |
| `isConnected()` was true once the broker accepted the connection | It is true once the broker has also acknowledged every subscription; nothing is published before then |
| `sendGatewayConnectEvent(name)` published the bare name | It publishes `{"device":"name"}` as ThingsBoard expects, and sub-devices are connected again after every reconnect |
| `requestAttributesJson()` callbacks waited forever | A request without an answer is dropped after `VL_ATTR_TIMEOUT_MS` (10 s), still silently. At most `VL_COMPAT_MAX_REQUESTS` (4) can be outstanding |
| System attributes `upTime`, `ESP Free Heap`, `ESP32 temperature` every 10 s | `sendSystemAttributes(true)` turns on `vl::Health`: `vl_uptime`, `vl_heap_free` and the other `vl_*` keys, still every 10 s by default. Set `VL_COMPAT_V1_KEYS=1` to keep `upTime`, `ESP Free Heap` and `Viralink SDK Version`. The chip temperature is no longer reported |
| `MQTTOTA` accepted MD5 only, and `chunkSize` set the chunk | Every ThingsBoard checksum algorithm is accepted. Chunks are sized from the transport buffer, so `chunkSize` is ignored. `checkForUpdate()` has nothing to do, because the package is read on every connect and every `fw_*` update. `stopHandleOTAMessages()` holds downloads (`vl::Ota::hold()`); a download it interrupted starts over after `startHandleOTAMessages()` |
| `setBufferSize()`, `setTimeout()` resized PubSubClient | The buffer is fixed at `VL_TX_BUFFER_SIZE` (4 KB on ESP32), and reconnects back off with jitter. These calls do nothing |

The shim parses every incoming message into a `JsonDocument` for the 1.0.2
callbacks, so it uses the heap where 1.0.2 did. The v2 API underneath does
not.

## Route 2: porting to the v2 API

| 1.0.2 | v2 |
| --- | --- |
| `#include "viralink.h"` | `#include <ViraLink.h>` |
| `MQTTController mqtt(512)` | `vl::RingOutbox<16 * 1024> outbox;` plus a transport (`vl::EspMqtt`, or `vl::BufferedStreamMqtt<>` over `vl::ArduinoStream(netClient)`) and `vl::Client client(transport, outbox)` |
| `mqtt.connect(netClient, id, token, "", host, port, cb, raw, onConnected)` | `vl::Config cfg; cfg.host = host; cfg.clientId = id; cfg.auth = vl::AccessToken(token); client.begin(cfg);` with `client.onState()` for the connection event and modules (below) in place of `cb` |
| `mqtt.connect(…, id, user, pass, …)` | `cfg.auth = vl::MqttBasic(id, user, pass)` |
| `mqtt.loop()` on its own core | `client.loop()`, or `client.startTask(core)` on ESP32 |
| `mqtt.isConnected()` | `client.online()` |
| `mqtt.sendTelemetry(doc)` | `client.telemetry().add("temp", 21.5).send()`, or `client.sendTelemetryJson(json)` |
| `mqtt.sendAttributes(doc)` | `client.attributes().add("fw", "1.0").send()`, or `client.sendAttributesJson(json)` |
| `mqtt.sendClaimRequest(key, ms)` | `client.claim(key, ms)` |
| RPC in the default callback, reply with `addToPublishQueue("…/rpc/response/" + id, …)` | `vl::Rpc<N> rpc; rpc.on("method", handler);` and the handler fills `RpcReply`; unknown methods are answered automatically |
| Shared-attribute updates in the default callback | `vl::SharedAttributes<N> shared;` with `shared.onUpdate("a,b", cb)` or `shared.bind("interval", &var)` |
| `mqtt.requestAttributesJson("{\"sharedKeys\":\"a\"}", cb)` | `shared.request(nullptr, "a", cb, ctx)`; `cb` also fires on timeout |
| `mqtt.subscribeToGatewayEvent()` | `vl::Gateway<N> gw; client.use(gw);` with `gw.on("method", handler)` and `gw.onAttributeUpdate(cb)` |
| `mqtt.sendTelemetry(doc, "Meter-1")` | `gw.device("Meter-1").telemetry().add("kwh", 3.5).send()` (connects the sub-device) |
| `mqtt.sendGatewayConnectEvent(name)` / `DisConnectEvent(name)` | `gw.device(name)` / `gw.disconnect(name)` |
| `mqtt.sendSystemAttributes(true)` and `updateSendSystemAttributesInterval(s)` | `vl::Health health(s * 1000); client.use(health);` |
| `mqtt.getQueueSize()` | `client.stats().queuedRecords` |
| `MQTTOTA ota(&mqtt, 10240); ota.begin(title, version)` | `vl::Ota ota; ota.begin(title, version); client.use(ota);` |
| `ota.stopHandleOTAMessages()` / `startHandleOTAMessages()` | `ota.hold(true)` / `ota.hold(false)` |
| `NetworkInterfacesController` with `NetworkInterface`s | `vl::Networks<N>` with `vl::EspNetif` (WiFi, Ethernet) and `vl::LinkNetwork` (TinyGSM) |
| `Uptime.getMilliseconds()` | `client.uptimeMs()` |

Attach every module before `client.begin()`: `client.use(rpc, shared, ota, health)`.

## Upgrading devices in the field

- **First update over the air.** 1.0.2 accepts only MD5, so the first
  package that brings v2 onto a fielded device must be uploaded with an
  MD5 checksum. Packages after that can use SHA-256.
- **Partitions.** v2 writes the image to the next OTA partition, as 1.0.2
  did through `Update`. Keep the partition table the devices already have.
- **Dashboards.** Health keys changed from `upTime` and `ESP Free Heap` to
  `vl_uptime` and `vl_heap_free`. Update the widgets, or build with
  `VL_COMPAT_V1_KEYS=1`.
- **Device profile.** The profile must use the JSON payload type and the
  default MQTT topic filters, as it did with 1.0.2.
