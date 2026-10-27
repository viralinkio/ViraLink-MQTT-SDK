// An STM32 with a W5500 Ethernet module and a W25Q SPI NOR flash on one SPI
// bus. The flash holds what must survive a power cut:
//   - the credentials provisioning returns (FlashKvStore, 8 KB), and
//   - the outbox (FlashOutboxStore, 8 x 4 KB), so records produced while the
//     server is unreachable are delivered after a restart.
// A power cut during a write leaves the old or the new credentials, and
// loses at most the records saved since the last flush.
//
// The STM32 has no wall clock: set one with vl::setUtcMs() from NTP, a modem
// or an RTC. Until then, records go without "ts" and the server stamps them.
//
// Boards: nucleo_f401re
// Libraries: arduino-libraries/Ethernet@^2.0.2

#include <Ethernet.h>
#include <SPI.h>
#include <ViraLink.h>

static const char* VL_HOST = "console.viralink.ir";
static const char* PROVISION_KEY = "YOUR_PROVISION_KEY";
static const char* PROVISION_SECRET = "YOUR_PROVISION_SECRET";

static const uint8_t ETH_CS = 10;
static const uint8_t FLASH_CS = 9;
static uint8_t MAC[6] = {0x02, 0x56, 0x4C, 0x00, 0x00, 0x01};  // locally administered

EthernetClient net;
vl::ArduinoStream stream(net);
vl::BufferedStreamMqtt<> mqtt(stream);
vl::RingOutbox<8 * 1024> outbox;
vl::Client client(mqtt, outbox);

vl::ArduinoSpi flashSpi(SPI, FLASH_CS);
vl::W25qFlash flash(flashSpi);
vl::FlashRegion kvArea(flash, 0, 2 * 4096);
vl::FlashRegion outboxArea(flash, 64 * 1024, 8 * 4096);
vl::FlashKvStore credentials(kvArea);
vl::FlashOutboxStore outboxStore(outboxArea, 8, 4096);
vl::Persistence persist(outboxStore);
vl::Provisioning prov(&credentials);
vl::Health health;

static void sendReading(void* /*ctx*/) { client.telemetry().add("adc", analogRead(A0)).send(); }

static void onProvisioned(vl::Result r, const char* error, void* /*ctx*/) {
  Serial.print("provisioning: ");
  Serial.print(vl::toString(r));
  if (error) {
    Serial.print(" ");
    Serial.print(error);
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  SPI.begin();
  pinMode(ETH_CS, OUTPUT);  // both chips deselected before either is used
  digitalWrite(ETH_CS, HIGH);
  pinMode(FLASH_CS, OUTPUT);
  digitalWrite(FLASH_CS, HIGH);

  if (flash.begin() != vl::Result::Ok) Serial.println("no SPI flash: running from RAM only");
  Ethernet.init(ETH_CS);
  Ethernet.begin(MAC);  // DHCP; blocks until it answers or gives up

  // The flash must be started before persistence attaches.
  client.use(persist, health);
  if (persist.status() != vl::Result::Ok) Serial.println("outbox not persistent");
  client.use(prov);
  client.onState([](vl::State s, void*) { Serial.println(vl::toString(s)); }, nullptr);
  client.every(10000, sendReading, nullptr);

  prov.setDevice(nullptr, PROVISION_KEY, PROVISION_SECRET);
  prov.onDone(onProvisioned, nullptr);
  vl::Config cfg;
  cfg.host = VL_HOST;
  cfg.port = 1883;
  prov.start(client, cfg);
}

void loop() {
  Ethernet.maintain();
  client.loop();
}
