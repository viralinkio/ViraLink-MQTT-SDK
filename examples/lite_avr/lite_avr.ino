// The Lite profile on an Arduino Mega 2560 with a W5100/W5500 Ethernet
// shield: telemetry, a client attribute and one server-side RPC over plain
// MQTT. Lite is chosen automatically on AVR; it drops OTA, the gateway,
// provisioning, persistence and TLS to fit in 8 KB of RAM.
//
// An AVR has no wall clock, so the server timestamps telemetry on arrival.
// Records still wait in the outbox until the broker acknowledges them.

#include <SPI.h>
#include <Ethernet.h>
#include <ViraLink.h>

static byte MAC[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0x01};
static const char* VL_HOST = "console.viralink.ir";
static const char* VL_TOKEN = "YOUR_DEVICE_TOKEN";
static const int RELAY_PIN = 7;

EthernetClient net;
vl::ArduinoStream stream(net);
vl::BufferedStreamMqtt<> mqtt(stream);  // VL_TX_BUFFER_SIZE: 256 bytes on Lite
vl::RingOutbox<1024> outbox;
vl::Client client(mqtt, outbox);
vl::Rpc<1> rpc;
vl::ClientAttributes<1> reported;
int32_t relay = 0;

static void setRelay(const vl::RpcRequest& req, vl::RpcReply& reply, void*) {
  relay = req.params()["on"].asBool() ? 1 : 0;
  digitalWrite(RELAY_PIN, relay ? HIGH : LOW);
  reply.json().beginObject().key("on").value(relay != 0).endObject();
  reply.send();
}

static void sample(void*) {
  client.telemetry().add("a0", analogRead(A0)).add("uptime_s", static_cast<uint32_t>(millis() / 1000)).send();
}

void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  Serial.begin(115200);
  if (Ethernet.begin(MAC) == 0) Serial.println(F("DHCP failed"));

  rpc.on("setRelay", setRelay);
  reported.report("relay", &relay);  // sent on change
  client.use(rpc, reported);
  client.every(10000, sample, nullptr);

  vl::Config cfg;
  cfg.host = VL_HOST;
  cfg.auth = vl::AccessToken(VL_TOKEN);
  client.begin(cfg);
}

void loop() {
  Ethernet.maintain();
  client.loop();
}
