// ViraLinkCompat.h end to end on the host: the 1.0.2 calls a sketch makes,
// over fake Arduino headers (test/support/arduino) and a scripted broker
// behind an Arduino Client.
#define ARDUINO 10819
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 0
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#define VL_COMPAT_GATEWAY 1

#include <string.h>
#include <unity.h>

#include <ViraLinkCompat.h>
#include <string>
#include <vector>

#include "Fakes.h"
#include "vl/core/Mqtt.h"

using vltest::FakeClock;

void setUp() {}
void tearDown() {}

namespace {

struct Pub {
  std::string topic, payload;
};

// An MQTT broker on the far side of an Arduino Client: accepts the CONNECT,
// grants every SUBSCRIBE and acknowledges every QoS 1 PUBLISH.
class FakeNet : public ::Client {
 public:
  bool up = false;
  int connects = 0, stops = 0;
  std::string clientId, user, pass;
  std::vector<std::string> subscribed;
  std::vector<Pub> published;

  int connect(const char*, uint16_t) override {
    ++connects;
    up = true;
    rx_.clear();
    tx_.clear();
    return 1;
  }
  size_t write(const uint8_t* p, size_t n) override {
    if (!up) return 0;
    rx_.append(reinterpret_cast<const char*>(p), n);
    parse();
    return n;
  }
  int available() override { return static_cast<int>(tx_.size()); }
  int read(uint8_t* buf, size_t n) override {
    size_t k = n < tx_.size() ? n : tx_.size();
    memcpy(buf, tx_.data(), k);
    tx_.erase(0, k);
    return static_cast<int>(k);
  }
  void stop() override {
    ++stops;
    up = false;
  }
  uint8_t connected() override { return up; }

  // A QoS 0 message from the server.
  void send(const char* topic, const std::string& payload) {
    uint8_t head[160];
    size_t n = vl::mqtt::publishHead(head, sizeof(head), topic, payload.size(), 0, 0);
    tx_.append(reinterpret_cast<const char*>(head), n);
    tx_ += payload;
  }
  std::vector<Pub> take() {
    std::vector<Pub> p = published;
    published.clear();
    return p;
  }

 private:
  static std::string str(const uint8_t*& p) {
    size_t n = static_cast<size_t>(p[0] << 8 | p[1]);
    std::string s(reinterpret_cast<const char*>(p + 2), n);
    p += 2 + n;
    return s;
  }
  void ack(uint8_t type, uint16_t id, int extra) {
    tx_ += static_cast<char>(type);
    tx_ += static_cast<char>(extra < 0 ? 2 : 3);
    tx_ += static_cast<char>(id >> 8);
    tx_ += static_cast<char>(id & 0xFF);
    if (extra >= 0) tx_ += static_cast<char>(extra);
  }
  void parse() {
    for (;;) {
      vl::mqtt::Header h;
      const uint8_t* p = reinterpret_cast<const uint8_t*>(rx_.data());
      if (vl::mqtt::header(p, rx_.size(), h) != vl::mqtt::Parse::Ok || rx_.size() < h.size + h.remaining) return;
      const uint8_t* body = p + h.size;
      if (h.type == 1) {  // CONNECT
        const uint8_t* q = body;
        str(q);  // "MQTT"
        uint8_t flags = q[1];
        q += 4;
        clientId = str(q);
        user = flags & 0x80 ? str(q) : "";
        pass = flags & 0x40 ? str(q) : "";
        tx_ += std::string("\x20\x02\x00\x00", 4);
      } else if (h.type == 8) {  // SUBSCRIBE
        const uint8_t* q = body + 2;
        subscribed.push_back(str(q));
        ack(0x90, static_cast<uint16_t>(body[0] << 8 | body[1]), 1);
      } else if (h.type == 3) {  // PUBLISH
        const uint8_t* topic;
        const uint8_t* payload;
        size_t topicLen, payloadLen;
        uint8_t qos;
        uint16_t id;
        TEST_ASSERT_TRUE(vl::mqtt::publish(h.flags, body, h.remaining, topic, topicLen, qos, id, payload, payloadLen));
        Pub m;
        m.topic.assign(reinterpret_cast<const char*>(topic), topicLen);
        m.payload.assign(reinterpret_cast<const char*>(payload), payloadLen);
        published.push_back(m);
        if (qos == 1) ack(0x40, id, -1);
      } else if (h.type == 12) {  // PINGREQ
        tx_ += std::string("\xD0\x00", 2);
      }
      rx_.erase(0, h.size + h.remaining);
    }
  }

  std::string rx_, tx_;
};

struct Calls {
  std::vector<std::string> log;
} calls;

bool onDefault(const String& topic, JsonDocument json) {
  calls.log.push_back(std::string("default ") + topic.c_str() + " " + json["method"].as<std::string>());
  return true;
}
bool onDefaultRaw(const String& topic, uint8_t* payload, unsigned int length) {
  calls.log.push_back(std::string("raw ") + topic.c_str() + " " + std::string(reinterpret_cast<char*>(payload), length));
  return true;
}
int connectedEvents = 0;
void onConnected() { ++connectedEvents; }

struct Rig {
  FakeNet net;
  FakeClock clock;
  MQTTController mqtt;
  Rig() : mqtt(1024) {
    calls.log.clear();
    connectedEvents = 0;
    clock.haveUtc = true;
    clock.utcAtZero = 1767225600000ull;  // uptime 1000 -> ...601000
    mqtt.client().setClock(clock);
  }
  void connect(::Client& c, const char* pass = "") {
    mqtt.connect(c, "esp", "TOKEN", pass, "console.viralink.io", 1883, onDefault, onDefaultRaw, onConnected);
    run();
  }
  void run(int n = 6) {
    for (int i = 0; i < n; ++i) mqtt.loop();
  }
};

bool has(const std::vector<std::string>& v, const char* s) {
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] == s) return true;
  }
  return false;
}

}  // namespace

static void test_connect_takes_effect_on_loop() {
  Rig r;
  r.mqtt.connect(r.net, "esp", "TOKEN", "", "console.viralink.io", 1883, onDefault, onDefaultRaw, onConnected);
  TEST_ASSERT_EQUAL(0, r.net.connects);  // nothing happens off the loop task
  r.run();
  TEST_ASSERT_EQUAL(1, r.net.connects);
  TEST_ASSERT_EQUAL_STRING("esp", r.net.clientId.c_str());
  TEST_ASSERT_EQUAL_STRING("TOKEN", r.net.user.c_str());
  TEST_ASSERT_EQUAL_STRING("", r.net.pass.c_str());
  TEST_ASSERT_TRUE(has(r.net.subscribed, "v1/devices/me/rpc/request/+"));
  TEST_ASSERT_TRUE(has(r.net.subscribed, "v1/devices/me/attributes"));
  TEST_ASSERT_TRUE(has(r.net.subscribed, "v1/devices/me/attributes/response/+"));
  TEST_ASSERT_TRUE(r.mqtt.isConnected());
  TEST_ASSERT_EQUAL(1, connectedEvents);
}

static void test_basic_credentials_with_a_password() {
  Rig r;
  r.connect(r.net, "secret");
  TEST_ASSERT_EQUAL_STRING("esp", r.net.clientId.c_str());
  TEST_ASSERT_EQUAL_STRING("TOKEN", r.net.user.c_str());
  TEST_ASSERT_EQUAL_STRING("secret", r.net.pass.c_str());
}

static void test_sends_reach_the_broker() {
  Rig r;
  r.connect(r.net);
  JsonDocument doc;
  doc["temp"] = 21.5;
  TEST_ASSERT_TRUE(r.mqtt.sendTelemetry(doc));
  TEST_ASSERT_TRUE(r.mqtt.sendTelemetry(String("{\"hum\":40}")));
  JsonDocument attrs;
  attrs["Phone"] = "123";
  TEST_ASSERT_TRUE(r.mqtt.sendAttributes(attrs));
  TEST_ASSERT_TRUE(r.mqtt.addToPublishQueue("v1/devices/me/rpc/response/5", "{\"ok\":true}"));
  TEST_ASSERT_TRUE(r.mqtt.sendClaimRequest("key", 60000));
  TEST_ASSERT_FALSE(r.mqtt.addToPublishQueue("some/other/topic", "{}"));
  TEST_ASSERT_EQUAL_UINT(5, r.mqtt.getQueueSize());
  r.run();
  std::vector<Pub> p = r.net.take();
  TEST_ASSERT_EQUAL_UINT(5, p.size());
  TEST_ASSERT_EQUAL_STRING("v1/devices/me/rpc/response/5", p[0].topic.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"ts\":1767225601000,\"values\":{\"temp\":21.5}}", p[1].payload.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"ts\":1767225601000,\"values\":{\"hum\":40}}", p[2].payload.c_str());
  TEST_ASSERT_EQUAL_STRING("v1/devices/me/attributes", p[3].topic.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"Phone\":\"123\"}", p[3].payload.c_str());
  TEST_ASSERT_EQUAL_STRING("v1/devices/me/claim", p[4].topic.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"secretKey\":\"key\",\"durationMs\":60000}", p[4].payload.c_str());
  TEST_ASSERT_EQUAL_UINT(0, r.mqtt.getQueueSize());
}

bool rawFirst(const String& topic, uint8_t*, unsigned int) {
  calls.log.push_back(std::string("rawFirst ") + topic.c_str());
  return strstr(topic.c_str(), "/rpc/") != nullptr;  // takes RPC only
}
bool jsonFirst(String topic, JsonDocument json) {
  calls.log.push_back(std::string("json ") + topic.c_str());
  return json["mode"].is<const char*>();
}

static void test_incoming_messages_follow_the_1_0_2_order() {
  Rig r;
  r.mqtt.registerCallbackRawPayload(rawFirst);
  r.mqtt.registerCallbackJsonPayload(jsonFirst);
  r.connect(r.net);
  r.net.send("v1/devices/me/rpc/request/1", "{\"method\":\"restart_device\"}");
  r.net.send("v1/devices/me/attributes", "{\"mode\":\"eco\"}");
  r.net.send("v1/devices/me/attributes", "{\"other\":1}");
  r.net.send("v1/devices/me/attributes", "not json");
  r.run();
  const char* want[] = {
      "rawFirst v1/devices/me/rpc/request/1",
      "rawFirst v1/devices/me/attributes",
      "json v1/devices/me/attributes",
      "rawFirst v1/devices/me/attributes",
      "json v1/devices/me/attributes",
      "default v1/devices/me/attributes null",
      "raw v1/devices/me/attributes {\"other\":1}",
      "rawFirst v1/devices/me/attributes",
      "raw v1/devices/me/attributes not json",
  };
  TEST_ASSERT_EQUAL_UINT(sizeof(want) / sizeof(want[0]), calls.log.size());
  for (size_t i = 0; i < calls.log.size(); ++i) TEST_ASSERT_EQUAL_STRING(want[i], calls.log[i].c_str());
}

std::vector<std::string> responses;
bool onAttrs(String topic, JsonDocument json) {
  responses.push_back(std::string(topic.c_str()) + " " + json["shared"]["a"].as<std::string>());
  return true;
}

static void test_attribute_request_with_and_without_callback() {
  Rig r;
  responses.clear();
  r.connect(r.net);
  TEST_ASSERT_TRUE(r.mqtt.requestAttributesJson("{\"sharedKeys\":\"a\"}", onAttrs));
  TEST_ASSERT_TRUE(r.mqtt.requestAttributesJson("{\"sharedKeys\":\"b\"}"));
  TEST_ASSERT_FALSE(r.mqtt.requestAttributesJson("a,b", onAttrs));
  r.run();
  std::vector<Pub> p = r.net.take();
  TEST_ASSERT_EQUAL_UINT(2, p.size());
  std::string id0 = p[0].topic.substr(strlen("v1/devices/me/attributes/request/"));
  std::string id1 = p[1].topic.substr(strlen("v1/devices/me/attributes/request/"));
  r.net.send(("v1/devices/me/attributes/response/" + id0).c_str(), "{\"shared\":{\"a\":\"x\"}}");
  r.net.send(("v1/devices/me/attributes/response/" + id1).c_str(), "{\"shared\":{\"b\":\"y\"}}");
  r.run();
  TEST_ASSERT_EQUAL_UINT(1, responses.size());
  TEST_ASSERT_EQUAL_STRING(("v1/devices/me/attributes/response/" + id0 + " x").c_str(), responses[0].c_str());
  // Without a callback the response goes down the default chain.
  TEST_ASSERT_EQUAL_UINT(2, calls.log.size());
  TEST_ASSERT_EQUAL_STRING(("default v1/devices/me/attributes/response/" + id1 + " null").c_str(),
                           calls.log[0].c_str());
}

static void test_connect_again_switches_the_network() {
  FakeNet gsm;  // outlives the controller, as a sketch's global would
  Rig r;
  r.connect(r.net);
  r.mqtt.connect(gsm, "esp", "TOKEN", "", "console.viralink.io", 1883, onDefault);
  r.run();
  TEST_ASSERT_TRUE(r.net.stops >= 1);
  TEST_ASSERT_FALSE(r.net.up);
  TEST_ASSERT_EQUAL(1, gsm.connects);
  TEST_ASSERT_TRUE(r.mqtt.isConnected());
  TEST_ASSERT_TRUE(r.mqtt.sendTelemetry(String("{\"t\":1}")));
  r.run();
  TEST_ASSERT_EQUAL_UINT(1, gsm.take().size());
  // The new connect() dropped the connection event.
  TEST_ASSERT_EQUAL(1, connectedEvents);
}

static void test_system_attributes_are_opt_in() {
  {
    Rig r;
    r.connect(r.net);
    TEST_ASSERT_EQUAL_UINT(0, r.net.take().size());
  }
  Rig r;
  r.mqtt.sendSystemAttributes(true);
  r.mqtt.updateSendSystemAttributesInterval(5);
  r.connect(r.net);
  std::vector<Pub> p = r.net.take();
  TEST_ASSERT_EQUAL_UINT(1, p.size());
  TEST_ASSERT_EQUAL_STRING("v1/devices/me/attributes", p[0].topic.c_str());
  TEST_ASSERT_TRUE(p[0].payload.find("\"vl_sdk\":\"" VL_SDK_VERSION "\"") != std::string::npos);
  r.clock.advance(5000);
  r.run();
  TEST_ASSERT_EQUAL_UINT(1, r.net.take().size());
}

static void test_ota_reports_firmware_and_holds() {
  Rig r;
  MQTTOTA ota(&r.mqtt, 10240);
  TEST_ASSERT_TRUE(ota.begin("viralink-test", "1"));
  r.connect(r.net);
  TEST_ASSERT_TRUE(has(r.net.subscribed, "v2/fw/response/+/chunk/+"));
  std::vector<Pub> p = r.net.take();
  bool info = false;
  for (size_t i = 0; i < p.size(); ++i) {
    info |= p[i].payload.find("\"current_fw_title\":\"viralink-test\",\"current_fw_version\":\"1\"") !=
            std::string::npos;
  }
  TEST_ASSERT_TRUE(info);
  TEST_ASSERT_TRUE(ota.checkForUpdate());
  ota.stopHandleOTAMessages();
  TEST_ASSERT_TRUE(ota.ota().held());
  ota.startHandleOTAMessages();
  TEST_ASSERT_FALSE(ota.ota().held());
}

static void test_gateway_mode() {
  Rig r;
  r.connect(r.net);
  TEST_ASSERT_TRUE(has(r.net.subscribed, "v1/gateway/rpc"));
  JsonDocument doc;
  doc["kwh"] = 3.5;
  TEST_ASSERT_TRUE(r.mqtt.sendTelemetry(doc, "Meter-1"));
  r.run();
  std::vector<Pub> p = r.net.take();
  TEST_ASSERT_EQUAL_UINT(2, p.size());
  TEST_ASSERT_EQUAL_STRING("{\"device\":\"Meter-1\"}", p[0].payload.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"Meter-1\":[{\"ts\":1767225601000,\"values\":{\"kwh\":3.5}}]}", p[1].payload.c_str());

  // Until subscribeToGatewayEvent() the gateway answers sub-device RPC itself.
  const char* rpc = "{\"device\":\"Meter-1\",\"data\":{\"id\":1,\"method\":\"m\",\"params\":{}}}";
  r.net.send("v1/gateway/rpc", rpc);
  r.run();
  TEST_ASSERT_EQUAL_UINT(0, calls.log.size());
  p = r.net.take();
  TEST_ASSERT_EQUAL_UINT(1, p.size());
  TEST_ASSERT_EQUAL_STRING("v1/gateway/rpc", p[0].topic.c_str());
  TEST_ASSERT_TRUE(r.mqtt.subscribeToGatewayEvent());
  r.net.send("v1/gateway/rpc", rpc);
  r.run();
  TEST_ASSERT_EQUAL_UINT(2, calls.log.size());
  TEST_ASSERT_EQUAL_STRING("default v1/gateway/rpc null", calls.log[0].c_str());
  TEST_ASSERT_EQUAL_UINT(0, r.net.take().size());

  TEST_ASSERT_TRUE(r.mqtt.sendGatewayDisConnectEvent("Meter-1"));
  r.run();
  p = r.net.take();
  TEST_ASSERT_EQUAL_UINT(1, p.size());
  TEST_ASSERT_EQUAL_STRING("v1/gateway/disconnect", p[0].topic.c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_connect_takes_effect_on_loop);
  RUN_TEST(test_basic_credentials_with_a_password);
  RUN_TEST(test_sends_reach_the_broker);
  RUN_TEST(test_incoming_messages_follow_the_1_0_2_order);
  RUN_TEST(test_attribute_request_with_and_without_callback);
  RUN_TEST(test_connect_again_switches_the_network);
  RUN_TEST(test_system_attributes_are_opt_in);
  RUN_TEST(test_ota_reports_firmware_and_holds);
  RUN_TEST(test_gateway_mode);
  return UNITY_END();
}
