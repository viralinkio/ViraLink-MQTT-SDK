# ViraLink Device Client v2

ThingsBoard-compatible MQTT device client for ESP32, ESP8266 and STM32 with an at-least-once
offline outbox. Works with ViraLink Console and any ThingsBoard CE 3.6+
server. Design: [`docs/design/`](docs/design/).

**Status: 2.0.0-alpha.1.** Done: session, outbox, telemetry and attributes,
scheduler, server- and client-side RPC, attribute requests, shared-attribute
updates and binding, client-attribute reporting, device claiming, the
gateway API, OTA, provisioning, health, persistence, TLS with ATECC608 keys,
the esp-mqtt and Arduino `Client` transports, network failover, the ESP8266
and STM32 ports, and SPI NOR flash storage. Still to come: OTA on STM32
(MCUboot) and the AVR port.

## Install

| Build system | How |
| --- | --- |
| PlatformIO (Arduino or ESP-IDF framework) | `lib_deps = https://github.com/viralinkio/ViraLink-Device-Client.git` |
| Arduino IDE | Sketch → Include Library → Add .ZIP Library (needs arduino-esp32 ≥ 2.0, the ESP8266 core ≥ 3.1, or STM32duino ≥ 2.11) |
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
`examples/persistence` (Arduino), `examples/arduino_client` (Arduino),
`examples/secure_mtls` (Arduino), `examples/multi_network` (Arduino),
`examples/ota_arduino` (Arduino), `examples/stm32_flash` (Arduino, STM32),
`examples/espidf_basic_telemetry` (ESP-IDF). `arduino_client` and
`ota_arduino` also build for ESP8266; the others are ESP32 sketches.

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
- On ESP8266 the image goes to the free flash after the sketch and the
  bootloader copies it over at the restart; there is no rollback.
- `vl::StreamHttp` downloads URL packages over any Arduino `Client`: the
  default on ESP8266 (which has no built-in source), and an alternative to
  esp_http_client on ESP32. It sends HTTP/1.0, resumes with `Range` and
  follows up to 3 redirects (never from `https` to `http`). An `https://` URL
  needs the second, TLS stream,
  which the application configures:

  ```cpp
  WiFiClient web;
  WiFiClientSecure webTls;                        // setTrustAnchors() / setCACert()
  vl::ArduinoStream webStream(web), webTlsStream(webTls);
  vl::StreamHttp http(webStream, &webTlsStream);  // null: http:// only
  ota.setHttp(http);
  ```

- Other targets need an `IFirmwareWriter` passed to `ota.setWriter()`, and
  `ota.setHttp()` for URL packages.
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

### Persistence

Queued records survive restarts and power cuts when the outbox is saved to a
store:

```cpp
vl::FileOutboxStore store("/littlefs/vl", 16, 4096);  // after LittleFS.begin(); dir must exist
vl::Persistence persist(store);
client.use(persist);  // before begin(): restores what an earlier boot saved
```

- Restored records go out first, in their original order. Their uptime
  timestamps belonged to the earlier boot, so they are sent without `ts`
  and the server stamps them (records already stamped in UTC keep it).
- Data records are saved once 16 are waiting or 5 s after a `loop()` first
  sees one (`VL_OUTBOX_FLUSH_RECORDS`, `VL_OUTBOX_FLUSH_MS`), so a power cut
  loses at most that window. Control records (claims, `fw_state`, gateway
  connect/disconnect) are saved on the next `loop()`. RPC replies and
  claims (which carry a secret, and must not be replayed after a reboot)
  are never saved. Call `persist.flush()` before deep sleep.
- Gateway records are saved with their sub-device's name and find it again
  after a reboot. Attach the gateway before persistence,
  `client.use(gw, persist)`; otherwise they are dropped with a warning.
- Telemetry is stored as written. Enable ESP32 flash encryption if queued
  data must not be readable from a removed flash chip.
- Delivery is at least once: a record acknowledged shortly before a power
  cut may be sent again after it.
- The store is a log of CRC-checked entries in segments, recovered up to the
  last intact entry. It needs at least 3 segments and twice the ring's
  capacity. `FileOutboxStore` keeps one file per segment on any mounted
  ESP32 file system (LittleFS recommended); `FlashOutboxStore` puts the
  segments straight on SPI NOR flash (see SPI NOR flash); other media
  implement `vl::IOutboxStore`. A store error is logged and the client
  carries on from RAM.
- Saving runs on the client task, outside the lock, so producers never wait
  for flash.

### Health

`vl::Health` reports the device's vitals as client attributes:

```cpp
vl::Health health;                 // every VL_HEALTH_INTERVAL_MS (60 s)
health.setKeys(vl::kHealthAll & ~vl::kHealthRssi);  // optional: switch keys off
client.use(health);
```

| Key | Value |
| --- | --- |
| `vl_uptime` | seconds since boot |
| `vl_heap_free`, `vl_heap_min` | free heap now and its low-water mark, bytes |
| `vl_reset_reason` | `power_on`, `software`, `panic`, `task_wdt`, `brownout`, `deep_sleep`, ... |
| `vl_rssi`, `vl_iface` | signal strength (dBm) and name of the active interface |
| `vl_queue_depth` | records waiting in the outbox |
| `vl_dropped`, `vl_dead_letter` | data records dropped when the ring was full; poison records discarded |
| `vl_sdk` | library version |

- Every connection sends all enabled keys; each interval after that sends
  all but `vl_sdk` and `vl_reset_reason`, which do not change during a boot.
  `vl::Health health(0)` reports only on connect. Nothing is queued offline.
- On ESP32, `vl_rssi` and `vl_iface` describe the WiFi station while it is
  associated. Other targets, or a custom link, supply values through an
  `vl::ISystemInfo` passed to `health.setSource()`; unknown values are left out.
- Upgrading from 1.0.2: set `VL_COMPAT_V1_KEYS` to 1 (or call
  `health.useV1Keys(true)`) to keep the old names `upTime`, `ESP Free Heap`
  and `Viralink SDK Version`, so existing dashboards keep working. Keys
  1.0.2 did not have keep their `vl_` names.

### Transports and TLS

`vl::EspMqtt` (esp-mqtt) is the ESP32 transport, on Arduino and ESP-IDF
alike. `vl::BufferedStreamMqtt<>` speaks MQTT 3.1.1 itself over a byte
stream, so the client also runs over any Arduino network `Client`:

```cpp
WiFiClient net;                         // or EthernetClient, TinyGsmClient
vl::ArduinoStream stream(net);
vl::BufferedStreamMqtt<> mqtt(stream);  // in place of vl::EspMqtt
```

- QoS 1 with any number of publishes awaiting PUBACK, and keep-alive pings;
  the link is dropped after 1.5 keep-alive periods of silence. An inbound
  message larger than the buffer (`VL_TX_BUFFER_SIZE`) is acknowledged,
  skipped and counted in `mqtt.droppedMessages()`.
- Arduino clients connect synchronously, so `loop()` blocks while one does.
- A TLS client you configure yourself (such as SSLClient) works too; leave
  `Config::tls` null then.

For TLS, `Config::tls` names the CA, and for mTLS the client certificate and
key. Pin the CA, not the server certificate: ThingsBoard 4.2.2.2 and later
rotate server certificates without a restart.

```cpp
vl::PemTls tls(CA_PEM);                      // or PemTls(CA_PEM, CERT_PEM, KEY_PEM) for mTLS
cfg.port = 8883;
cfg.tls = &tls;
```

The client key can live in an ATECC608A/B, which it never leaves:

| Build | How |
| --- | --- |
| ESP-IDF 5.x, or Arduino as an IDF component | `EspMqtt` with `CONFIG_ESP_TLS_USE_SECURE_ELEMENT` and the `espressif/esp-cryptoauthlib` component; key slot 0 and the I2C address come from Kconfig |
| Arduino-ESP32 2.0.x | `vl::AteccTlsStream` over any Arduino `Client` (`examples/secure_mtls`) |
| Arduino-ESP32 3.x | Not supported: mbedTLS 3 hides the key hook |

```cpp
WiFiClient tcp;
vl::WireI2c i2c(Wire);                            // after Wire.begin()
vl::AteccTlsStream tls(tcp, i2c);
vl::BufferedStreamMqtt<> mqtt(tls);
vl::SecureElementTls identity(CA_PEM, CERT_PEM);  // key in slot 0 of the chip at 0x60
cfg.auth = vl::X509();
cfg.tls = &identity;
```

- `begin()` returns `Rejected`, and logs why, when the transport cannot do
  what `Config::tls` asks: a secure element where the build has no support
  for one, a slot or address other than Kconfig's, TLS on a plain stream.
- `AteccTlsStream` refuses a certificate whose public key is not the chip's,
  and offers TLS 1.2 ECDHE suites only. The key slot must allow signing
  external messages (Trust&GO slot 0 does). mbedTLS allocates about 40 KB at
  connect, freed when the connection closes.
- Certificate dates: `AteccTlsStream` checks them against the client clock,
  both dates once it is set (SNTP), before that only expiry before
  `VL_TIME_FLOOR_S`. esp-tls checks dates only on ESP-IDF builds with
  `CONFIG_MBEDTLS_HAVE_TIME_DATE`, which makes TLS wait for SNTP; Arduino's
  prebuilt `EspMqtt` does not check them.

### Several networks

`vl::Networks<N>` lets the client use several networks in priority order. It
is the client's transport, forwarding to the transport of the network in use:

```cpp
vl::EspMqtt mqtt;
vl::EspNetif ethernet("eth", "ETH_DEF", mqtt);    // LAN8720 and other PHYs
vl::EspNetif wifi("wifi", "WIFI_STA_DEF", mqtt);  // one EspMqtt for both: lwIP interfaces
vl::Networks<3> nets;
vl::Client client(nets, outbox);

nets.add(ethernet);       // best first
nets.add(wifi);
health.setSource(nets);   // vl_iface / vl_rssi: the network in use
```

- When the network in use goes down, the session moves to the best one that
  is up at once. It moves back to a better network only after that one has
  stayed up for 30 s without a break (`Timing::failBackMs`), so a flapping
  link does not bounce it. Records not yet acknowledged are sent again over
  the new network.
- A network that is up but reaches no broker (WiFi without an uplink) is
  left after 120 s without a CONNACK when another one is up, and may not win
  the session back for 5 minutes (`stallMs`, `retryMs`). Any CONNACK, even a
  refusal, counts as reaching the broker.
- When no network is up the client waits; the moment one comes up it
  connects, without waiting out its backoff (once per outage, and never
  inside the 30 s a CONNACK 3 asks for).
- `EspNetif` takes any ESP32 lwIP interface by key (`WIFI_STA_DEF`, `ETH_DEF`,
  `PPP_DEF`); before each connect it makes that interface lwIP's default
  route. lwIP has one DNS server list for all interfaces, so when they sit on
  different LANs use a public resolver.
- A modem through TinyGSM brings its own transport,
  `BufferedStreamMqtt` over `ArduinoStream(gsmClient)`, wrapped in
  `vl::LinkNetwork("gsm", gsmMqtt, upFn, ctx, 5000)`; the last argument runs
  a slow check (an AT command) at most every 5 s. Keeping the modem attached
  is the application's job.
- Every network's transport must accept `Config::tls`, and the largest
  publish is the smallest any of them allows.

### ESP8266

The ESP8266 core (3.1 or later) runs the client over `BufferedStreamMqtt`
and a `WiFiClient` (`examples/arduino_client`):

- Uptime comes from `micros64()` and wall-clock time from `configTime()`;
  the client ID is built from the station MAC, as on ESP32.
- Health reports free heap, reset reason, `wifi` and RSSI. The lowest free
  heap needs `UMM_STATS_FULL`, so it is left out.
- `vl::FsStore` keeps provisioned credentials in a mounted file system, one
  file per key, replaced atomically on LittleFS:

  ```cpp
  LittleFS.begin();
  vl::FsStore store(LittleFS);  // files under /vl
  ```

- OTA: the platform firmware writer is used by default; set a `StreamHttp`
  for URL packages (see OTA).
- Not available: `startTask()` (the core has no RTOS), the file-backed
  outbox (no POSIX file API), ATECC608 keys and `EspMqtt`.
- The client loop must run often: StreamHttp and Arduino connects block,
  and yield to the core's watchdog while they do.

### STM32

STM32duino (2.11 or later) runs the client over `BufferedStreamMqtt` and any
Arduino `Client`: a W5500 `EthernetClient`, a `TinyGsmClient`, or
`STM32Ethernet` on boards with a MAC (`examples/stm32_flash`).

- The client ID comes from the chip's 96-bit unique ID, so it is stable
  across boots. Backoff jitter uses the RNG peripheral where the chip has
  one and its clock runs, and a generator seeded from the unique ID
  elsewhere (F1, F401, F411 and others have no RNG).
- There is no wall clock: call `vl::setUtcMs(ms)` once the application has
  the time (NTP over UDP, a modem's `AT+CCLK`, an RTC). Until then records
  go without `ts`; those still queued are stamped when it is set. Works on
  any target; on ESP32 and ESP8266 it sets the system time.
- Health reports free heap and the reset reason. The reason is read from
  the RCC flags at the first report, which then clears them (as
  `IWatchdog::isReset(true)` would).
- Credentials and the outbox go on an SPI NOR chip (next section). Internal
  flash is not used: its sectors are large and differ between families.
- With every module enabled, a sketch takes about 70 KB of flash and 14 KB
  of RAM (core included), so pick a part with 128 KB of flash or more: a
  64 KB Blue Pill does not fit. The `VL_ENABLE_*` switches in `Config.h`
  leave out the modules you do not use.
- Not available yet: OTA (MCUboot, next), `startTask()` (no RTOS is
  assumed; with STM32FreeRTOS, implement `vl::ILock` and call `loop()` from
  one task).

### SPI NOR flash

A W25Q (or GD25Q, MX25L: any JEDEC chip with 4 KB sectors) holds the
credentials store and the persistent outbox, on any target with SPI:

```cpp
vl::ArduinoSpi flashSpi(SPI, FLASH_CS);               // a transaction per command: the bus can be shared
vl::W25qFlash flash(flashSpi);
vl::FlashRegion kvArea(flash, 0, 2 * 4096);           // offsets and sizes in whole 4 KB sectors
vl::FlashRegion outboxArea(flash, 64 * 1024, 8 * 4096);
vl::FlashKvStore credentials(kvArea);                 // for vl::Provisioning
vl::FlashOutboxStore outboxStore(outboxArea, 8, 4096);
vl::Persistence persist(outboxStore);

void setup() {
  SPI.begin();
  flash.begin();         // identifies the chip; before client.use(persist)
  client.use(persist);
}
```

- `FlashKvStore` survives power cuts: a cut during a write leaves the old
  value or the new one. It needs two or more sectors and keeps an
  append-only log, compacted into its spare half when full. Writing the
  value already stored costs nothing.
- `FlashOutboxStore` maps each outbox segment onto whole sectors; programs
  are durable when they return, so nothing waits for a sync.
- Programs and erases block while the chip works (a sector erase can take
  400 ms) and yield meanwhile. Chips over 16 MB are used for their first
  16 MB. Values and records are stored in the clear.

### Claiming

`client.claim("secret", 60000)` lets a user claim the device in ViraLink
Console within 60 s, using the secret key (or none, with `nullptr`).

## Develop

```sh
pio test -e native            # host unit tests, ASan + UBSan
scripts/build_examples.sh     # every example: Arduino + ESP-IDF (PlatformIO), idf.py if present
scripts/check_portability.sh  # AVR, Cortex-M0+, ESP8266, ESP32-C3 compilers and Arduino cores
```

An example's `// Boards:` line lists the boards `build_examples.sh` builds it
for (`esp32dev` when it has none); `BOARDS=nodemcuv2` limits a run to some.

## License

Apache-2.0.
