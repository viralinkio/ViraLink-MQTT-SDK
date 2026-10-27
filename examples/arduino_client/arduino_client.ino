// The client over any Arduino network Client instead of esp-mqtt: here a
// plain WiFiClient, but an EthernetClient (W5500) or a TinyGsmClient plugs in
// the same way. StreamMqtt speaks MQTT 3.1.1 itself (QoS 1, keep-alive).
//
// For TLS, wrap the Client in a TLS client you configure yourself (such as
// SSLClient) and leave Config::tls null: the library cannot see inside it.
//
// connect() blocks while the Client connects; on ESP32, run client.loop()
// from its own task if the sketch cannot wait.
//
// Boards: esp32dev nodemcuv2

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif
#include <ViraLink.h>
#include <time.h>

static const char* WIFI_SSID = "your-ssid";
static const char* WIFI_PASS = "your-password";
static const char* VL_HOST = "console.viralink.ir";
static const char* VL_TOKEN = "YOUR_DEVICE_TOKEN";

WiFiClient net;
vl::ArduinoStream stream(net);
vl::BufferedStreamMqtt<> mqtt(stream);  // buffer of VL_TX_BUFFER_SIZE bytes
vl::RingOutbox<16 * 1024> outbox;
vl::Client client(mqtt, outbox);

static void sendClimate(void*) {
  client.telemetry().add("light", analogRead(A0)).send();
}

void setup() {
  Serial.begin(115200);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, "pool.ntp.org");

  client.onState([](vl::State s, void*) { Serial.printf("state: %s\n", vl::toString(s)); }, nullptr);
  client.every(10000, sendClimate, nullptr);

  vl::Config cfg;
  cfg.host = VL_HOST;
  cfg.port = 1883;
  cfg.auth = vl::AccessToken(VL_TOKEN);
  client.begin(cfg);
}

void loop() {
  client.loop();
  delay(5);
  if (mqtt.droppedMessages()) Serial.printf("messages too large for the buffer: %lu\n",
                                           static_cast<unsigned long>(mqtt.droppedMessages()));
}
