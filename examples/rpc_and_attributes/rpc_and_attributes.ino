// Server-side RPC and shared attributes through registered callbacks, and a
// client attribute sent on change.
//
// - RPC "setRelay" {"on":true} drives a relay and answers {"on":true}.
// - Shared attributes "interval" (ms) and "mode" arrive in onShared(), both
//   as updates and in the answer to the request sent on every connect.
// - Client attribute "door" is sent when it changes, at most once a second.
// - Telemetry follows the current interval.

#include <WiFi.h>
#include <ViraLink.h>
#include <time.h>

static const char* WIFI_SSID = "your-ssid";
static const char* WIFI_PASS = "your-password";
static const char* VL_HOST = "console.viralink.ir";
static const char* VL_TOKEN = "YOUR_DEVICE_TOKEN";

static const int RELAY_PIN = 26;
static const int DOOR_PIN = 27;
static const int32_t DEFAULT_INTERVAL = 10000;

vl::EspMqtt mqtt;
vl::RingOutbox<16 * 1024> outbox;
vl::Client client(mqtt, outbox);
vl::Rpc<4> rpc;
vl::SharedAttributes<2> shared;

// Written by the callbacks, which run on the client task (here: loop()).
int32_t interval = DEFAULT_INTERVAL;
char mode[16] = "normal";

static void setRelay(const vl::RpcRequest& req, vl::RpcReply& reply, void* /*ctx*/) {
  vl::JsonValue on = req.params()["on"];
  if (on.type() != vl::JsonValue::Type::Bool) {
    reply.error("params.on must be true or false");
    return;
  }
  digitalWrite(RELAY_PIN, on.asBool() ? HIGH : LOW);
  reply.json().beginObject().key("on").value(on.asBool()).endObject();
  reply.send();
}

// value is Invalid when the attribute was deleted: fall back to the default.
static void onShared(const char* key, vl::JsonValue value, void* /*ctx*/) {
  if (strcmp(key, "interval") == 0) {
    long long ms = value.asInt(DEFAULT_INTERVAL);
    interval = ms >= 1000 && ms <= 86400000 ? static_cast<int32_t>(ms) : DEFAULT_INTERVAL;
  } else if (strcmp(key, "mode") == 0) {
    if (!value.copyString(mode, sizeof(mode))) strcpy(mode, "normal");
  }
  Serial.printf("shared %s: interval=%ld mode=%s\n", key, static_cast<long>(interval), mode);
}

// Answer to the request below: the current values, fed through onShared().
static void onSharedValues(vl::Result r, vl::JsonValue /*client*/, vl::JsonValue values, void* /*ctx*/) {
  if (r != vl::Result::Ok) {
    Serial.printf("shared attributes not fetched: %s\n", vl::toString(r));
    return;
  }
  char key[32];
  vl::JsonValue k, v;
  for (size_t i = 0; values.member(i, k, v); ++i) {
    if (k.copyString(key, sizeof(key))) onShared(key, v, nullptr);
  }
}

static void onState(vl::State s, void* /*ctx*/) {
  Serial.printf("state: %s\n", vl::toString(s));
  // Updates only report changes; fetch the current values after each connect.
  if (s == vl::State::Online) shared.request(nullptr, "interval,mode", onSharedValues, nullptr);
}

// Runs every second and sends when the interval has elapsed, so a new
// interval takes effect without rescheduling.
static void maybeSendClimate(void* /*ctx*/) {
  static uint32_t last = 0;
  if (millis() - last < static_cast<uint32_t>(interval)) return;
  last = millis();
  vl::Result r = client.telemetry().add("temp", temperatureRead()).add("mode", mode).send();
  if (!vl::ok(r)) Serial.printf("telemetry not queued: %s\n", vl::toString(r));
}

// Sends the door state when it changed, at most once a second.
static void reportDoor() {
  static int sent = -1;
  static uint32_t lastMs = 0;
  int door = digitalRead(DOOR_PIN) == HIGH;  // open when the reed switch releases
  if (door == sent || (sent >= 0 && millis() - lastMs < 1000)) return;
  if (!vl::ok(client.attributes().add("door", door).send())) return;  // retried next loop
  sent = door;
  lastMs = millis();
}

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(DOOR_PIN, INPUT_PULLUP);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, "pool.ntp.org");

  rpc.on("setRelay", setRelay);
  shared.onUpdate("interval,mode", onShared);
  client.use(rpc, shared);

  client.onState(onState, nullptr);
  client.every(1000, maybeSendClimate, nullptr);

  vl::Config cfg;
  cfg.host = VL_HOST;
  cfg.auth = vl::AccessToken(VL_TOKEN);
  client.begin(cfg);
}

void loop() {
  reportDoor();
  client.loop();
  delay(5);
}
