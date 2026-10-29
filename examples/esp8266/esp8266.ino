// The client on an ESP8266 (NodeMCU, Wemos D1 mini): telemetry, one
// server-side RPC, health attributes and OTA, over WiFi with StreamMqtt.
//
// - Time comes from SNTP (configTime); telemetry queued before the first
//   sync is stamped once the clock is known.
// - OTA uses the core's Updater: packages made from a file arrive over MQTT
//   chunks; packages with fw_url are fetched over HTTP, or HTTPS with the CA
//   given to ota.setHttpTls(). ESP8266 has no rollback: the new image runs
//   as soon as eboot has copied it.

#include <ESP8266WiFi.h>
#include <ViraLink.h>
#include <time.h>

static const char* WIFI_SSID = "your-ssid";
static const char* WIFI_PASS = "your-password";
static const char* VL_HOST = "console.viralink.ir";
static const char* VL_TOKEN = "YOUR_DEVICE_TOKEN";

WiFiClient net;
vl::ArduinoStream stream(net);
vl::BufferedStreamMqtt<> mqtt(stream);  // VL_TX_BUFFER_SIZE: 1 KB here
vl::RingOutbox<8 * 1024> outbox;
vl::Client client(mqtt, outbox);
vl::Rpc<2> rpc;
vl::Health health;
vl::Ota ota;

static void blink(const vl::RpcRequest& req, vl::RpcReply& reply, void*) {
  bool on = req.params()["on"].asBool();
  digitalWrite(LED_BUILTIN, on ? LOW : HIGH);  // active low
  reply.json().beginObject().key("on").value(on).endObject();
  reply.send();
}

static void sample(void*) {
  client.telemetry().add("a0", analogRead(A0)).add("rssi", WiFi.RSSI()).send();
}

static void onOta(vl::OtaState s, const char* error, void*) {
  Serial.printf("ota: %s %s\n", vl::toString(s), error ? error : "");
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, "pool.ntp.org");

  rpc.on("blink", blink);
  ota.begin("esp8266-demo", "1.0.0");
  ota.onState(onOta, nullptr);
  client.use(rpc, health, ota);
  client.onState([](vl::State s, void*) { Serial.printf("state: %s\n", vl::toString(s)); }, nullptr);
  client.every(10000, sample, nullptr);

  vl::Config cfg;
  cfg.host = VL_HOST;
  cfg.auth = vl::AccessToken(VL_TOKEN);
  client.begin(cfg);
}

void loop() {
  client.loop();  // blocks while StreamMqtt connects
}
