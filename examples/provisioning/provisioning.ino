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
