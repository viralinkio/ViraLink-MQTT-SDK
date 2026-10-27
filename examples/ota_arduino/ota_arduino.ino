// Firmware updates on any Arduino network Client: MQTT through StreamMqtt,
// and packages made from a URL (fw_url) downloaded by StreamHttp over a
// plain or a TLS client. The image goes to the platform's firmware writer:
// the next OTA slot on ESP32, the free flash after the sketch on ESP8266.
//
// - Upload a package with title "ota-demo" and a newer version, then assign
//   it to this device. A package made from a file arrives in MQTT chunks.
// - fw_url packages need a SHA-2 checksum. Redirects (up to 3) are followed;
//   an https:// URL uses webTls, which trusts CA_PEM below.
// - StreamHttp blocks client.loop() for up to VL_OTA_HTTP_TIMEOUT_MS while a
//   server is slow to answer.
//
// Build the new firmware with FW_VERSION changed to the package's version.
//
// Boards: esp32dev nodemcuv2

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#include <WiFiClientSecure.h>
#endif
#include <ViraLink.h>
#include <time.h>

static const char* WIFI_SSID = "your-ssid";
static const char* WIFI_PASS = "your-password";
static const char* VL_HOST = "console.viralink.ir";
static const char* VL_TOKEN = "YOUR_DEVICE_TOKEN";

static const char* FW_TITLE = "ota-demo";
static const char* FW_VERSION = "1.0.0";

// Root CA of the server that hosts fw_url packages.
static const char CA_PEM[] = R"PEM(
-----BEGIN CERTIFICATE-----
...
-----END CERTIFICATE-----
)PEM";

WiFiClient net;  // MQTT
vl::ArduinoStream stream(net);
vl::BufferedStreamMqtt<> mqtt(stream);
vl::RingOutbox<8 * 1024> outbox;
vl::Client client(mqtt, outbox);
vl::Ota ota;

WiFiClient web;           // http:// downloads
WiFiClientSecure webTls;  // https:// downloads
vl::ArduinoStream webStream(web);
vl::ArduinoStream webTlsStream(webTls);
vl::StreamHttp http(webStream, &webTlsStream);
#if defined(ESP8266)
BearSSL::X509List trust(CA_PEM);
#endif

static void onOtaState(vl::OtaState state, const char* error, void* /*ctx*/) {
  if (error) {
    Serial.printf("ota: %s (%s)\n", vl::toString(state), error);
  } else {
    Serial.printf("ota: %s\n", vl::toString(state));
  }
}

void setup() {
  Serial.begin(115200);
  Serial.printf("running %s %s\n", FW_TITLE, FW_VERSION);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, "pool.ntp.org");  // certificate checks need the time

#if defined(ESP8266)
  webTls.setTrustAnchors(&trust);
#else
  webTls.setCACert(CA_PEM);
#endif

  ota.begin(FW_TITLE, FW_VERSION);
  ota.setHttp(http);
  ota.onState(onOtaState, nullptr);
  client.use(ota);

  client.onState([](vl::State s, void*) { Serial.printf("state: %s\n", vl::toString(s)); }, nullptr);

  vl::Config cfg;
  cfg.host = VL_HOST;
  cfg.port = 1883;
  cfg.auth = vl::AccessToken(VL_TOKEN);
  client.begin(cfg);
}

void loop() {
  client.loop();
  delay(5);
}
