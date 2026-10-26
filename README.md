# ViraLink Device Client v2

ThingsBoard-compatible MQTT device client for ESP32 with an at-least-once
offline outbox. Works with ViraLink Console and any ThingsBoard CE 3.6+
server. Design: [`docs/design/`](docs/design/).

**Status: 2.0.0-alpha.1.** Done: session, outbox, telemetry and attributes,
scheduler, server- and client-side RPC, attribute requests, shared-attribute
updates and binding, client-attribute reporting, device claiming, the
gateway API, and the esp-mqtt transport. Still to come: OTA, provisioning,
health, persistence, and ESP8266/STM32/AVR transports.

## Install

| Build system | How |
| --- | --- |
| PlatformIO (Arduino or ESP-IDF framework) | `lib_deps = https://github.com/viralinkio/ViraLink-Device-Client.git` |
| Arduino IDE | Sketch → Include Library → Add .ZIP Library (needs arduino-esp32 ≥ 2.0) |
| ESP-IDF (`idf.py`, IDF ≥ 4.4) | Clone into `components/`, or add the path to `EXTRA_COMPONENT_DIRS` |

## Use

```cpp
#include <ViraLink.h>

vl::EspMqtt mqtt;                  // esp-mqtt transport (Arduino and ESP-IDF)
vl::RingOutbox<16 * 1024> outbox;  // records stay here until PUBACK
vl::Client client(mqtt, outbox);

void setup() {
  vl::Config cfg;
  cfg.host = "console.viralink.ir";
  cfg.auth = vl::AccessToken("YOUR_DEVICE_TOKEN");
  client.begin(cfg);
  client.every(10000, [](void*) { client.telemetry().add("temp", 21.5).send(); }, nullptr);
}

void loop() { client.loop(); }     // or client.startTask() on ESP32
```

Telemetry is timestamped at enqueue time. Records produced before the wall
clock is set are restamped when it syncs, so offline data keeps its real time.

Examples: `examples/basic_telemetry` (Arduino), `examples/rpc_and_attributes` (Arduino),
`examples/gateway` (Arduino), `examples/ota` (Arduino), `examples/provisioning` (Arduino),
`examples/espidf_basic_telemetry` (ESP-IDF).

### API modules

RPC and attributes are modules with fixed capacity, attached before `begin()`.
You consume what the server sends by registering functions on them:

```cpp
vl::Rpc<4> rpc;                  // up to 4 server-side methods
vl::SharedAttributes<4> shared;  // up to 4 update handlers (and bindings)

client.use(rpc, shared);
```

Callbacks are plain function pointers with a `void* ctx` and run on the client
task. Incoming JSON arrives as a `vl::JsonValue`, a view into the payload that
is valid only during the callback; copy what you keep.

### RPC

```cpp
rpc.on("setRelay", [](const vl::RpcRequest& req, vl::RpcReply& reply, void*) {
  bool on = req.params()["on"].asBool();
  digitalWrite(RELAY_PIN, on);
  reply.json().beginObject().key("on").value(on).endObject();
  reply.send();                    // or reply.send("{...}"), reply.error("...")
});
```

Every server RPC gets an answer: unknown methods and handlers that send nothing
are answered with `{"error":...}`, so the server never waits out its timeout.
Replies go out ahead of queued telemetry.

Client-side RPC: `rpc.call("getCurrentTime", nullptr, cb, ctx)` publishes the
request and fires `cb(result, response, ctx)` once, with `Result::Timeout` after
`VL_RPC_TIMEOUT_MS`. At most `VL_RPC_MAX_CALLS` calls are outstanding.

### Attributes

```cpp
// Shared-attribute updates for these keys (nullptr = every key).
shared.onUpdate("interval,mode", [](const char* key, vl::JsonValue value, void*) {
  if (!value.valid()) return;    // the attribute was deleted
  if (strcmp(key, "interval") == 0) interval = value.asInt(10000);
});

// Current values on demand: client keys, shared keys, callback.
shared.request("fw", "interval,mode", [](vl::Result r, vl::JsonValue client, vl::JsonValue values, void*) {
  if (r == vl::Result::Ok) interval = values["interval"].asInt(10000);
}, nullptr);

// Client attributes are sent like telemetry.
client.attributes().add("door", 1).send();
```

Updates carry only what changed, so request the current values after each
connect (in the `onState` callback when the state is `Online`), as
`examples/rpc_and_attributes` does.

Optional helpers, for when a plain variable is all you need:

```cpp
shared.bind("interval", &interval);   // bool, int32_t, float or char buffer
vl::ClientAttributes<2> reported;     // attach with client.use(...)
reported.report("door", &door, 1000); // on change, at most once a second
```

A bound variable is requested on every connect, rewritten on each update and
reset to `0`, `false` or `""` when deleted; values of another type are ignored.
A reported variable is read on every loop and sent when it changes.

### Gateway

One MQTT session can speak for several sub-devices (the device must have "Is gateway" set on the server):

```cpp
vl::Gateway<16> gw;                               // up to 16 sub-devices
client.use(gw);

gw.on("setValve", [](const char* /*device*/, const vl::RpcRequest& req, vl::RpcReply& reply, void*) {
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

### OTA

Firmware updates use ThingsBoard OTA packages: assign one to the device or
its profile and the device installs it.

```cpp
vl::Ota ota;
ota.begin("ViraGate", "2.3.0");  // the running firmware's title and version
ota.onState([](vl::OtaState s, const char* error, void*) { /* log, LEDs */ }, nullptr);
client.use(ota);
```

- A package made from a file arrives over MQTT in chunks as large as the
  transport buffer allows; a package made from a URL (`fw_url`) is
  downloaded over HTTP(S), resuming with a `Range` request after a break.
  HTTPS trusts `Config::tls`, or `ota.setHttpTls()`, or on ESP32 the
  certificate bundle.
- Every checksum algorithm ThingsBoard offers is checked while the image is
  written: MD5, SHA256, SHA384, SHA512, CRC32, MURMUR3_32 and MURMUR3_128.
  A package without a checksum is refused unless `VL_OTA_ALLOW_UNVERIFIED` is 1.
  A URL package must use SHA256, SHA384 or SHA512: its image arrives outside
  the MQTT session, where CRC32, Murmur3 or MD5 would not stop a forged image.
  To also reject images the server itself did not build, enable signed app
  images (`CONFIG_SECURE_SIGNED_APPS_*`) on ESP32: `esp_ota_end` then checks
  the signature.
- `fw_state` goes through DOWNLOADING, DOWNLOADED, VERIFIED and UPDATING;
  the device then restarts into the new image, which reports UPDATED. A
  failure reports FAILED with `fw_error`. A failed package is not tried
  again until it is reassigned, another one is assigned, or the device
  restarts.
- On ESP32 the image goes to the next OTA partition. With
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, a new image is confirmed on its
  first connection and rolled back if it crashes or is not online within
  `VL_OTA_CONFIRM_TIMEOUT_MS` (5 minutes); the package it came from then
  reports FAILED "rolled back" instead of being installed again.
- Other targets need an `IFirmwareWriter` (and an `IHttpSource` for URL
  packages) passed to `ota.setWriter()` / `ota.setHttp()`.
- The download blocks `loop()` for at most one chunk write, or one HTTP read
  of `VL_OTA_HTTP_BLOCK` bytes (up to `VL_OTA_HTTP_TIMEOUT_MS` on a stalled link).

### Provisioning

A device can create itself on the server with a device profile's provision
key and secret, then keep the credentials it gets back:

```cpp
vl::NvsStore store;                // ESP32 NVS; any vl::IKeyValueStore works
vl::Provisioning prov(&store);

prov.setDevice("Meter-7", "PROVISION_KEY", "PROVISION_SECRET");  // name null: the server picks
prov.onDone([](vl::Result r, const char* error, void*) { /* Ok, or Rejected with the server's message */ }, nullptr);
prov.start(client, cfg);           // instead of client.begin(cfg)
```

- With saved credentials `start()` begins the normal session at once.
  Otherwise it runs a short provisioning session first: username
  `provision`, only `/provision/response` subscribed (without waiting for a
  SUBACK, which ThingsBoard 3.6-3.8 never send), one request, and on success
  the credentials are saved and the normal session starts with them. Other
  modules and queued telemetry wait for that session.
- By default the server generates an access token. The device can bring its
  own with `prov.setCredentials(vl::AccessToken(token))`,
  `vl::MqttBasic(clientId, username, password)`, or `vl::X509()` plus
  `prov.setCertificate(pem)` and a `Config::tls` that presents that certificate.
- A refusal (for example a wrong key) or no answer within
  `VL_PROVISION_TIMEOUT_MS` (30 s) stops the client and reports the error.
- `prov.forget()` erases the saved credentials; call it from
  `client.onAuthFailed` to provision again after the device was deleted.
- On ESP32, enable NVS encryption to keep the credentials encrypted at rest.

### Claiming

`client.claim("secret", 60000)` lets a user claim the device in ViraLink
Console within 60 s, using the secret key (or none, with `nullptr`).

## Develop

```sh
pio test -e native            # host unit tests, ASan + UBSan
scripts/build_examples.sh     # every example: Arduino + ESP-IDF (PlatformIO), idf.py if present
```

## License

Apache-2.0.
