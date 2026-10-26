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
