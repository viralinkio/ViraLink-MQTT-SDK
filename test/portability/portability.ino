// Links the whole library for a microcontroller with no network hardware: a
// transport that never connects, every module attached, every value type
// sent. Built by scripts/check_portability.sh; never flashed.
#include <Arduino.h>
#include <ViraLink.h>

class NullTransport : public vl::IMqttTransport {
 public:
  void beginConnect(const vl::ConnectOptions&) override {}
  void disconnect() override {}
  bool connected() const override { return false; }
  bool publish(const char*, const uint8_t*, size_t, uint8_t, uint16_t*) override { return false; }
  void subscribe(const char*, uint8_t) override {}
  void loop() override {}
  size_t maxPacket() const override { return VL_TX_BUFFER_SIZE; }
  void onConnack(ConnackCb, void*) override {}
  void onSuback(SubackCb, void*) override {}
  void onPuback(PubackCb, void*) override {}
  void onMessage(MessageCb, void*) override {}
  void onClosed(ClosedCb, void*) override {}
};

NullTransport mqtt;
vl::RingOutbox<1024> outbox;
vl::Client client(mqtt, outbox);
vl::Rpc<2> rpc;
vl::SharedAttributes<4> shared;
int32_t intervalMs = 10000;
bool enabled = true;
float gain = 1.0f;
char mode[8] = "auto";
vl::ClientAttributes<2> reported;
int32_t door = 0;
#if VL_ENABLE_GATEWAY
vl::Gateway<2> gw;
#endif
#if VL_ENABLE_HEALTH
vl::Health health(30000);
#endif
#if VL_ENABLE_PERSISTENCE
// A RAM-backed store, so the journal links without a file system.
class RamStore : public vl::IOutboxStore {
 public:
  size_t segmentCount() const override { return 4; }
  size_t segmentSize() const override { return sizeof(mem_[0]); }
  vl::Result read(size_t seg, size_t off, uint8_t* buf, size_t n, size_t& got) override {
    got = off < sizeof(mem_[0]) ? (n < sizeof(mem_[0]) - off ? n : sizeof(mem_[0]) - off) : 0;
    memcpy(buf, mem_[seg] + off, got);
    return vl::Result::Ok;
  }
  vl::Result write(size_t seg, size_t off, const uint8_t* p, size_t n) override {
    memcpy(mem_[seg] + off, p, n);
    return vl::Result::Ok;
  }
  vl::Result erase(size_t seg) override {
    memset(mem_[seg], 0xFF, sizeof(mem_[0]));
    return vl::Result::Ok;
  }
  vl::Result sync(size_t) override { return vl::Result::Ok; }

 private:
  uint8_t mem_[4][1024];
};
RamStore ramStore;
vl::Persistence persist(ramStore);
#endif
#if VL_ENABLE_PROVISION
vl::Provisioning prov;  // no store: provisions on every boot

static void onProvisioned(vl::Result r, const char* error, void*) {
  Serial.println(vl::toString(r));
  if (error) Serial.println(error);
}
#endif
#if VL_ENABLE_OTA
vl::Ota ota;

static void onOta(vl::OtaState state, const char* error, void*) {
  Serial.println(vl::toString(state));
  if (error) Serial.println(error);
}
#endif

static void onMode(const char* key, vl::JsonValue value, void*) {
  char mode[8];
  if (value.copyString(mode, sizeof(mode))) Serial.println(key);
}

static void onFw(vl::Result r, vl::JsonValue client, vl::JsonValue, void*) {
  if (r == vl::Result::Ok) Serial.println(static_cast<long>(client["fw"].asInt()));
}

static void onSetRelay(const vl::RpcRequest& req, vl::RpcReply& reply, void*) {
  reply.json().beginObject().key("on").value(req.params()["on"].asBool()).endObject();
  reply.send();
}

static void sample(void*) {
  client.telemetry()
      .add("f", 21.5f)
      .add("d", 0.25)
      .add("i32", static_cast<int32_t>(-7))
      .add("u32", static_cast<uint32_t>(7))
      .add("i64", static_cast<int64_t>(-1))
      .add("b", true)
      .add("s", "x")
      .send();
}

void setup() {
  Serial.begin(115200);
  vl::JsonValue v = vl::JsonValue::parse("{\"a\":1}", 7);
  client.use(rpc, shared, reported);
#if VL_ENABLE_HEALTH
  health.setKeys(vl::kHealthAll & ~vl::kHealthRssi);
  client.use(health);
#endif
#if VL_ENABLE_PERSISTENCE
  client.use(persist);
  persist.flush();
#endif
#if VL_ENABLE_GATEWAY
  client.use(gw);
  gw.device("Meter-1", "meter").telemetry().add("kwh", 1.5f).send();
#endif
#if VL_ENABLE_OTA
  ota.begin("portability", "1.0.0");
  ota.onState(onOta, nullptr);
  client.use(ota);
#endif
  reported.report("door", &door, 1000);
  reported.report("gain", &gain);
  rpc.on("setRelay", onSetRelay);
  shared.onUpdate("mode", onMode);
  shared.bind("interval", &intervalMs);
  shared.bind("enabled", &enabled);
  shared.bind("gain", &gain);
  shared.bind("mode", mode, sizeof(mode));
  shared.request("fw", nullptr, onFw, nullptr);
  client.every(1000, sample, nullptr);
  vl::Config cfg;
  cfg.host = "example.org";
  cfg.auth = vl::AccessToken("TOKEN");
#if VL_ENABLE_PROVISION
  prov.setDevice("portability", "KEY", "SECRET");
  prov.setCredentials(vl::MqttBasic("cid", "user", "pass"));
  prov.onDone(onProvisioned, nullptr);
  prov.start(client, cfg);
#else
  client.begin(cfg);
#endif
  Serial.println(static_cast<long>(v["a"].asInt()));
}

void loop() {
  door = digitalRead(2);
  client.loop();
}
