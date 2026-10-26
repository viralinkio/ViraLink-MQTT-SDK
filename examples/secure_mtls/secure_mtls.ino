// mTLS with the device key in an ATECC608A/B: the private key never leaves
// the chip, which signs the one handshake message that proves the device's
// identity. For Arduino-ESP32 2.0.x (mbedTLS 2.x); on ESP-IDF use EspMqtt with
// CONFIG_ESP_TLS_USE_SECURE_ELEMENT instead (see README, TLS).
//
// Before flashing:
//   - the chip holds a P-256 private key in KEY_SLOT (provisioned with
//     Microchip's tools; Trust&GO parts ship with one in slot 0)
//   - CLIENT_CERT is that key's certificate, registered on the device as
//     X.509 credentials; CA_CERT is the CA that signed the server certificate
//   - the chip is on the default I2C pins (SDA 21, SCL 22)
//
// connect() blocks during the TCP connect and the TLS handshake (a few
// seconds); run client.loop() from its own task if the sketch cannot wait.

#include <WiFi.h>
#include <Wire.h>
#include <ViraLink.h>
#include <time.h>

#if !VL_HAVE_ATECC_TLS
#error "AteccTlsStream needs Arduino-ESP32 2.0.x (mbedTLS 2.x)"
#endif

static const char* WIFI_SSID = "your-ssid";
static const char* WIFI_PASS = "your-password";
static const char* VL_HOST = "console.viralink.ir";
static const uint8_t ATECC_ADDRESS = 0x60;  // 7-bit; Trust&GO 0x35, TrustFLEX 0x36
static const uint8_t KEY_SLOT = 0;

static const char CA_CERT[] = R"(-----BEGIN CERTIFICATE-----
...the CA that signed the server certificate...
-----END CERTIFICATE-----
)";

static const char CLIENT_CERT[] = R"(-----BEGIN CERTIFICATE-----
...this device's certificate, for the key in KEY_SLOT...
-----END CERTIFICATE-----
)";

static vl::SecureElement atecc() {
  vl::SecureElement se;
  se.i2cAddress = ATECC_ADDRESS;
  se.keySlot = KEY_SLOT;
  return se;
}

WiFiClient tcp;
vl::WireI2c i2c(Wire);
vl::AteccTlsStream tls(tcp, i2c);  // TLS over the TCP client, key in the chip
vl::BufferedStreamMqtt<> mqtt(tls);  // MQTT over that stream
vl::RingOutbox<16 * 1024> outbox;
vl::Client client(mqtt, outbox);
vl::SecureElementTls identity(CA_CERT, CLIENT_CERT, atecc());

static void sendClimate(void*) {
  client.telemetry().add("temp", temperatureRead()).send();
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  // Certificate dates are checked against this clock once it is set; until
  // then only expiry before the firmware's floor date is caught.
  configTime(0, 0, "pool.ntp.org");

  client.onState([](vl::State s, void*) { Serial.printf("state: %s\n", vl::toString(s)); }, nullptr);
  client.every(10000, sendClimate, nullptr);

  vl::Config cfg;
  cfg.host = VL_HOST;
  cfg.port = 8883;
  cfg.auth = vl::X509();  // the certificate is the device's identity
  cfg.tls = &identity;
  vl::Result r = client.begin(cfg);
  if (r != vl::Result::Ok) Serial.printf("begin: %s\n", vl::toString(r));
}

void loop() {
  client.loop();
  delay(5);
}
