// Keep queued telemetry across restarts and power cuts. Records are saved to
// LittleFS through vl::Persistence; after a reboot they are restored ahead of
// new data and delivered once the device is back online.
//
// The default partition table's "spiffs" partition holds the file system.

#include <LittleFS.h>
#include <WiFi.h>
#include <ViraLink.h>

static const char* WIFI_SSID = "your-ssid";
static const char* WIFI_PASS = "your-password";
static const char* VL_HOST = "console.viralink.ir";
static const char* VL_TOKEN = "YOUR_DEVICE_TOKEN";

vl::EspMqtt mqtt;
vl::RingOutbox<16 * 1024> outbox;
vl::Client client(mqtt, outbox);
// 16 files of 4 KB: the store needs at least twice the ring.
vl::FileOutboxStore store("/littlefs/vl", 16, 4096);
vl::Persistence persist(store);

static void sample(void* /*ctx*/) {
  client.telemetry().add("temp", temperatureRead()).send();
}

void setup() {
  Serial.begin(115200);
  if (!LittleFS.begin(true) || !(LittleFS.exists("/vl") || LittleFS.mkdir("/vl"))) {
    Serial.println("LittleFS unavailable: queued data will not survive a restart");
  } else {
    client.use(persist);  // restores what an earlier boot saved
    Serial.printf("outbox store: %s, %u records restored\n", vl::toString(persist.status()),
                  static_cast<unsigned>(client.stats().queuedRecords));
  }

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, "pool.ntp.org");
  client.every(10000, sample, nullptr);

  vl::Config cfg;
  cfg.host = VL_HOST;
  cfg.auth = vl::AccessToken(VL_TOKEN);
  client.begin(cfg);
}

void loop() {
  client.loop();
  delay(5);
}
