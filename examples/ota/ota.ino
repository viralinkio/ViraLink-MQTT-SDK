// Firmware updates from ViraLink Console (ThingsBoard OTA packages).
//
// - Upload a package with title "ota-demo" and a newer version, then assign
//   it to this device (or its device profile). A package made from a file
//   arrives in MQTT chunks; one made from a URL is downloaded over HTTPS.
// - The image is checked against the package checksum, the new slot is
//   activated, and the device restarts into it. fw_state on the device's
//   page follows each step.
// - With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE (ESP-IDF menuconfig), an image
//   that never connects is rolled back after VL_OTA_CONFIRM_TIMEOUT_MS or a
//   crash; with the Arduino core's default bootloader it stays installed.
//
// Build the new firmware with FW_VERSION changed to the package's version.

#include <WiFi.h>
#include <ViraLink.h>
#include <time.h>

static const char* WIFI_SSID = "your-ssid";
static const char* WIFI_PASS = "your-password";
static const char* VL_HOST = "console.viralink.ir";
static const char* VL_TOKEN = "YOUR_DEVICE_TOKEN";

static const char* FW_TITLE = "ota-demo";
static const char* FW_VERSION = "1.0.0";

vl::EspMqtt mqtt;
vl::RingOutbox<8 * 1024> outbox;
vl::Client client(mqtt, outbox);
vl::Ota ota;

static uint32_t lastPrintMs = 0;

static void onOtaState(vl::OtaState state, const char* error, void* /*ctx*/) {
  if (error) {
    Serial.printf("ota: %s (%s)\n", vl::toString(state), error);
  } else {
    Serial.printf("ota: %s\n", vl::toString(state));
  }
}

static void onOtaProgress(size_t done, size_t total, void* /*ctx*/) {
  uint32_t now = millis();
  if (now - lastPrintMs < 2000 && done != total) return;
  lastPrintMs = now;
  if (total) {
    Serial.printf("ota: %u of %u bytes\n", static_cast<unsigned>(done), static_cast<unsigned>(total));
  } else {
    Serial.printf("ota: %u bytes\n", static_cast<unsigned>(done));
  }
}

void setup() {
  Serial.begin(115200);
  Serial.printf("running %s %s\n", FW_TITLE, FW_VERSION);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, "pool.ntp.org");

  ota.begin(FW_TITLE, FW_VERSION);
  ota.onState(onOtaState, nullptr);
  ota.onProgress(onOtaProgress, nullptr);
  client.use(ota);

  client.onState([](vl::State s, void*) { Serial.printf("state: %s\n", vl::toString(s)); }, nullptr);

  vl::Config cfg;
  cfg.host = VL_HOST;
  cfg.auth = vl::AccessToken(VL_TOKEN);
  client.begin(cfg);
}

void loop() {
  client.loop();
  delay(5);
}
