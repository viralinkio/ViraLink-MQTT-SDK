// Connect to ViraLink Console (or any ThingsBoard server), send timestamped
// telemetry every 10 s with client.every(), and report a client attribute.
//
// Records produced while offline wait in the outbox and are delivered, in
// order and with their original timestamps, once the link is back.

#include <WiFi.h>
#include <ViraLink.h>
#include <time.h>

static const char* WIFI_SSID = "your-ssid";
static const char* WIFI_PASS = "your-password";
static const char* VL_HOST = "console.viralink.ir";
static const char* VL_TOKEN = "YOUR_DEVICE_TOKEN";

vl::EspMqtt mqtt;                  // esp-mqtt, QoS1, runs its socket on its own task
vl::RingOutbox<16 * 1024> outbox;  // RAM ring
vl::Client client(mqtt, outbox);

static void sendClimate(void* /*ctx*/) {
  float temp = temperatureRead();  // internal sensor, stands in for a real one
  vl::Result r = client.telemetry().add("temp", temp).add("heap", ESP.getFreeHeap()).send();
  if (!vl::ok(r)) Serial.printf("telemetry not queued: %s\n", vl::toString(r));
}

void setup() {
  Serial.begin(115200);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, "pool.ntp.org");  // wall clock for "ts"; records are restamped once it syncs

  client.onState([](vl::State s, void*) { Serial.printf("state: %s\n", vl::toString(s)); }, nullptr);
  client.onAuthFailed([](void*) { Serial.println("token rejected: provision or load a new one"); }, nullptr);
  client.every(10000, sendClimate, nullptr);

  vl::Config cfg;
  cfg.host = VL_HOST;
  cfg.port = 1883;
  cfg.auth = vl::AccessToken(VL_TOKEN);
  client.begin(cfg);

  client.attributes().add("fw", "1.0.0").add("sdk", VL_SDK_VERSION).send();
}

void loop() {
  client.loop();
  delay(5);
}
