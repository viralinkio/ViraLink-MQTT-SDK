// Ethernet (LAN8720) first, WiFi as the fallback. When the active link goes
// down the session moves to the other one at once; it moves back only after
// the better link has stayed up for 30 s, so a flapping cable or access point
// does not bounce it. Records not yet acknowledged are sent again over the new
// link; vl_iface in the health attributes names the link in use.
//
// Both are lwIP interfaces, so one EspMqtt serves them: the network set makes
// the chosen one lwIP's default route before each connect. DNS: lwIP has one
// server list for both, so on separate LANs use a public resolver.

#include <ETH.h>
#include <WiFi.h>
#include <ViraLink.h>
#include <time.h>

static const char* WIFI_SSID = "your-ssid";
static const char* WIFI_PASS = "your-password";
static const char* VL_HOST = "console.viralink.ir";
static const char* VL_TOKEN = "YOUR_DEVICE_TOKEN";

vl::EspMqtt mqtt;
vl::EspNetif ethernet("eth", "ETH_DEF", mqtt);
vl::EspNetif wifi("wifi", "WIFI_STA_DEF", mqtt);
vl::Networks<3> nets;  // the client's transport: it forwards to the active network
vl::RingOutbox<16 * 1024> outbox;
vl::Client client(nets, outbox);
vl::Health health;

// GSM through TinyGSM is not an lwIP interface, so it brings its own transport
// (MQTT over the modem's Client). Added last, it is used only when Ethernet and
// WiFi are both down:
//
//   TinyGsm modem(Serial1);
//   TinyGsmClient gsmClient(modem);
//   vl::ArduinoStream gsmStream(gsmClient);
//   vl::BufferedStreamMqtt<> gsmMqtt(gsmStream);
//   vl::LinkNetwork gsm("gsm", gsmMqtt, [](void*) { return modem.isGprsConnected(); }, nullptr,
//                       5000);  // an AT command: check every 5 s at most
//   ...
//   nets.add(gsm);
//
// Keeping the modem registered and GPRS attached (modem.gprsConnect()) stays
// the sketch's job.

static void sendClimate(void*) {
  client.telemetry().add("temp", temperatureRead()).send();
}

static void onNetwork(const char* from, const char* to, void*) {
  Serial.printf("network: %s -> %s\n", from ? from : "none", to ? to : "none");
}

void setup() {
  Serial.begin(115200);
  ETH.begin();  // board defaults; WT32-ETH01: ETH.begin(1, 16, 23, 18, ETH_PHY_LAN8720, ETH_CLOCK_GPIO0_IN)
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, "pool.ntp.org");

  nets.add(ethernet);  // best first
  nets.add(wifi);
  nets.onChange(onNetwork, nullptr);
  health.setSource(nets);  // vl_iface and vl_rssi of the network in use
  client.use(health);
  client.every(10000, sendClimate, nullptr);

  vl::Config cfg;
  cfg.host = VL_HOST;
  cfg.auth = vl::AccessToken(VL_TOKEN);
  client.begin(cfg);
}

void loop() {
  client.loop();
  delay(5);
}
