// StreamMqtt over a scripted stream: connect, QoS1 publish and subscribe,
// inbound messages, split and merged reads, oversize packets, keep-alive,
// and every way the link can fail.
#include <string.h>
#include <unity.h>

#include <string>
#include <vector>

#include "Fakes.h"
#include "vl/core/Mqtt.h"
#include "vl/transport/StreamMqtt.h"

using namespace vl;
using vltest::FakeClock;

void setUp() {}
void tearDown() {}

namespace {

struct FakeStream : INetStream {
  bool accept = true;
  bool isSecure = false;
  bool hasSecureElement = false;
  bool gone = false;
  bool failWrite = false;
  size_t chunk = 1 << 20;  // most bytes one read returns
  std::string host;
  uint16_t port = 0;
  std::string in;   // bytes the broker sent, not yet read
  std::string out;  // bytes written
  int connects = 0;
  int stops = 0;

  bool connect(const char* h, uint16_t p) override {
    ++connects;
    host = h;
    port = p;
    gone = !accept;
    return accept;
  }
  int read(uint8_t* buf, size_t n) override {
    if (in.empty()) return gone ? -1 : 0;
    size_t k = std::min(std::min(n, chunk), in.size());
    memcpy(buf, in.data(), k);
    in.erase(0, k);
    return static_cast<int>(k);
  }
  bool write(const uint8_t* p, size_t n) override {
    if (failWrite || gone) return false;
    out.append(reinterpret_cast<const char*>(p), n);
    return true;
  }
  void stop() override { ++stops; }
  bool secure() const override { return isSecure; }
  bool secureElement() const override { return hasSecureElement; }
};

struct Events {
  std::vector<std::string> log;
};

void onConnack(uint8_t rc, void* ctx) { static_cast<Events*>(ctx)->log.push_back("connack " + std::to_string(rc)); }
void onSuback(const char* f, bool granted, void* ctx) {
  static_cast<Events*>(ctx)->log.push_back(std::string("suback ") + f + (granted ? " ok" : " refused"));
}
void onPuback(uint16_t id, void* ctx) { static_cast<Events*>(ctx)->log.push_back("puback " + std::to_string(id)); }
void onMessage(const char* topic, const uint8_t* p, size_t n, void* ctx) {
  static_cast<Events*>(ctx)->log.push_back(std::string("msg ") + topic + " " +
                                           std::string(reinterpret_cast<const char*>(p), n));
}
void onClosed(void* ctx) { static_cast<Events*>(ctx)->log.push_back("closed"); }

std::string packet(const uint8_t* p, size_t n) { return std::string(reinterpret_cast<const char*>(p), n); }

std::string publishPacket(const char* topic, const std::string& payload, uint8_t qos, uint16_t id) {
  uint8_t head[64];
  size_t n = mqtt::publishHead(head, sizeof(head), topic, payload.size(), qos, id);
  return packet(head, n) + payload;
}

std::string pubackPacket(uint16_t id) {
  uint8_t b[4];
  return packet(b, mqtt::puback(b, sizeof(b), id));
}

// Byte-exact: packets contain NULs, which string asserts would stop at.
void same(const std::string& expected, const std::string& got) {
  TEST_ASSERT_EQUAL_UINT(expected.size(), got.size());
  if (!expected.empty()) TEST_ASSERT_EQUAL_MEMORY(expected.data(), got.data(), expected.size());
}

const std::string kConnackOk("\x20\x02\x00\x00", 4);

struct Rig {
  FakeStream s;
  FakeClock clock;
  BufferedStreamMqtt<128> m;
  Events ev;
  ConnectOptions o;

  Rig() : m(s, &clock) {
    m.onConnack(onConnack, &ev);
    m.onSuback(onSuback, &ev);
    m.onPuback(onPuback, &ev);
    m.onMessage(onMessage, &ev);
    m.onClosed(onClosed, &ev);
    o.host = "broker";
    o.port = 1883;
    o.clientId = "vl-1";
    o.username = "TOKEN";
    o.keepAliveS = 10;
  }
  void online() {
    m.beginConnect(o);
    s.in += kConnackOk;
    m.loop();
    TEST_ASSERT_TRUE(m.connected());
    s.out.clear();
    ev.log.clear();
  }
  std::string events() {
    std::string r;
    for (size_t i = 0; i < ev.log.size(); ++i) r += (i ? " | " : "") + ev.log[i];
    ev.log.clear();
    return r;
  }
};

}  // namespace

static void test_connect_and_connack() {
  Rig r;
  r.m.beginConnect(r.o);
  TEST_ASSERT_EQUAL_STRING("broker", r.s.host.c_str());
  TEST_ASSERT_EQUAL_UINT16(1883, r.s.port);
  uint8_t expect[64];
  size_t n = mqtt::connect(expect, sizeof(expect), "vl-1", "TOKEN", nullptr, 10);
  same(packet(expect, n), r.s.out);
  TEST_ASSERT_FALSE(r.m.connected());
  r.s.in += std::string("\x20\x02\x00\x05", 4);  // refused: not authorised
  r.m.loop();
  TEST_ASSERT_EQUAL_STRING("connack 5", r.events().c_str());
  TEST_ASSERT_FALSE(r.m.connected());

  r.m.beginConnect(r.o);
  r.s.in += kConnackOk;
  r.m.loop();
  TEST_ASSERT_EQUAL_STRING("connack 0", r.events().c_str());
  TEST_ASSERT_TRUE(r.m.connected());
}

static void test_failed_connect_reported_from_loop() {
  Rig r;
  r.s.accept = false;
  r.m.beginConnect(r.o);
  TEST_ASSERT_EQUAL_STRING("", r.events().c_str());  // never from inside beginConnect
  r.m.loop();
  TEST_ASSERT_EQUAL_STRING("closed", r.events().c_str());
  r.m.loop();
  TEST_ASSERT_EQUAL_STRING("", r.events().c_str());  // once
}

static void test_publish_qos1_several_in_flight() {
  Rig r;
  TEST_ASSERT_FALSE(r.m.publish("t", reinterpret_cast<const uint8_t*>("x"), 1, 1, nullptr));  // not online
  r.online();
  uint16_t ids[3];
  for (int i = 0; i < 3; ++i) {
    std::string p = "p" + std::to_string(i);
    TEST_ASSERT_TRUE(r.m.publish("v1/devices/me/telemetry", reinterpret_cast<const uint8_t*>(p.data()), p.size(), 1,
                                 &ids[i]));
  }
  TEST_ASSERT_EQUAL_UINT16(1, ids[0]);
  TEST_ASSERT_EQUAL_UINT16(3, ids[2]);
  same(publishPacket("v1/devices/me/telemetry", "p0", 1, 1) + publishPacket("v1/devices/me/telemetry", "p1", 1, 2) +
           publishPacket("v1/devices/me/telemetry", "p2", 1, 3),
       r.s.out);
  r.s.in += pubackPacket(2) + pubackPacket(1) + pubackPacket(3);
  r.m.loop();
  TEST_ASSERT_EQUAL_STRING("puback 2 | puback 1 | puback 3", r.events().c_str());
  // QoS 0 carries no packet id.
  r.s.out.clear();
  TEST_ASSERT_TRUE(r.m.publish("t", reinterpret_cast<const uint8_t*>("z"), 1, 0, nullptr));
  same(publishPacket("t", "z", 0, 0), r.s.out);
}

static void test_subscribe_and_suback() {
  Rig r;
  r.online();
  r.m.subscribe("v1/devices/me/rpc/request/+", 1);
  r.m.subscribe("v1/devices/me/attributes", 1);
  uint8_t b[128];
  size_t n = mqtt::subscribe(b, sizeof(b), 1, "v1/devices/me/rpc/request/+", 1);
  size_t n2 = mqtt::subscribe(b + n, sizeof(b) - n, 2, "v1/devices/me/attributes", 1);
  TEST_ASSERT_EQUAL_UINT(n + n2, r.s.out.size());
  TEST_ASSERT_EQUAL_MEMORY(b, r.s.out.data(), n + n2);
  r.s.in += std::string("\x90\x03\x00\x02\x80\x90\x03\x00\x01\x01", 10);
  r.m.loop();
  TEST_ASSERT_EQUAL_STRING("suback v1/devices/me/attributes refused | suback v1/devices/me/rpc/request/+ ok",
                           r.events().c_str());
}

static void test_inbound_messages() {
  Rig r;
  r.online();
  r.s.in += publishPacket("v1/devices/me/rpc/request/7", "{\"method\":\"x\"}", 1, 0x0102) +
            publishPacket("v1/devices/me/attributes", "{\"a\":1}", 0, 0);
  r.m.loop();
  TEST_ASSERT_EQUAL_STRING(
      "msg v1/devices/me/rpc/request/7 {\"method\":\"x\"} | msg v1/devices/me/attributes {\"a\":1}",
      r.events().c_str());
  same(pubackPacket(0x0102), r.s.out);  // only the QoS 1 one
}

static void test_packets_split_across_reads() {
  Rig r;
  r.s.chunk = 1;
  r.m.beginConnect(r.o);
  r.s.in += kConnackOk + publishPacket("t/1", "hello", 1, 9);
  for (int i = 0; i < 40; ++i) r.m.loop();
  TEST_ASSERT_EQUAL_STRING("connack 0 | msg t/1 hello", r.events().c_str());
}

static void test_oversize_packet_skipped_but_acknowledged() {
  Rig r;
  r.online();
  r.s.chunk = 50;
  r.s.in += publishPacket("big", std::string(300, 'x'), 1, 77) + publishPacket("t", "ok", 0, 0);
  for (int i = 0; i < 20; ++i) r.m.loop();
  TEST_ASSERT_EQUAL_STRING("msg t ok", r.events().c_str());
  TEST_ASSERT_EQUAL_UINT32(1, r.m.droppedMessages());
  same(pubackPacket(77), r.s.out);
}

static void test_keep_alive_and_timeout() {
  Rig r;
  r.online();
  r.clock.advance(9999);
  r.m.loop();
  TEST_ASSERT_EQUAL_UINT(0, r.s.out.size());
  r.clock.advance(1);
  r.m.loop();
  same(std::string("\xC0\x00", 2), r.s.out);
  r.s.in += std::string("\xD0\x00", 2);  // PINGRESP
  r.clock.advance(1000);
  r.m.loop();
  TEST_ASSERT_TRUE(r.m.connected());
  // Silence for 1.5 keep-alive periods: the link is dead.
  r.clock.advance(14999);
  r.m.loop();
  TEST_ASSERT_TRUE(r.m.connected());
  r.clock.advance(1);
  r.m.loop();
  TEST_ASSERT_FALSE(r.m.connected());
  TEST_ASSERT_EQUAL_STRING("closed", r.events().c_str());
  TEST_ASSERT_EQUAL(1, r.s.stops);
}

static void test_link_failures_close() {
  Rig r;
  r.online();
  r.s.gone = true;
  r.m.loop();
  TEST_ASSERT_FALSE(r.m.connected());
  TEST_ASSERT_EQUAL_STRING("closed", r.events().c_str());

  r.s.gone = false;
  r.online();
  r.s.failWrite = true;
  TEST_ASSERT_FALSE(r.m.publish("t", reinterpret_cast<const uint8_t*>("x"), 1, 1, nullptr));
  TEST_ASSERT_FALSE(r.m.connected());
  r.m.loop();
  TEST_ASSERT_EQUAL_STRING("closed", r.events().c_str());

  r.s.failWrite = false;
  r.online();
  r.s.in += std::string("\x30\x80\x80\x80\x80\x01", 6);  // malformed length
  r.m.loop();
  TEST_ASSERT_FALSE(r.m.connected());
  TEST_ASSERT_EQUAL_STRING("closed", r.events().c_str());
}

static void test_disconnect_is_quiet() {
  Rig r;
  r.online();
  r.m.disconnect();
  same(std::string("\xE0\x00", 2), r.s.out);
  TEST_ASSERT_EQUAL(1, r.s.stops);
  TEST_ASSERT_FALSE(r.m.connected());
  r.m.loop();
  TEST_ASSERT_EQUAL_STRING("", r.events().c_str());
}

static void test_validate_against_the_stream() {
  Rig r;
  ConnectOptions o;
  TEST_ASSERT_EQUAL(Result::Ok, r.m.validate(o));
  PemTls pem("CA");
  o.tls = &pem;
  TEST_ASSERT_EQUAL(Result::Rejected, r.m.validate(o));  // plain stream: TLS would be ignored
  r.s.isSecure = true;
  TEST_ASSERT_EQUAL(Result::Ok, r.m.validate(o));
  SecureElementTls se("CA", "CERT");
  o.tls = &se;
  TEST_ASSERT_EQUAL(Result::Rejected, r.m.validate(o));
  r.s.hasSecureElement = true;
  TEST_ASSERT_EQUAL(Result::Ok, r.m.validate(o));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_connect_and_connack);
  RUN_TEST(test_failed_connect_reported_from_loop);
  RUN_TEST(test_publish_qos1_several_in_flight);
  RUN_TEST(test_subscribe_and_suback);
  RUN_TEST(test_inbound_messages);
  RUN_TEST(test_packets_split_across_reads);
  RUN_TEST(test_oversize_packet_skipped_but_acknowledged);
  RUN_TEST(test_keep_alive_and_timeout);
  RUN_TEST(test_link_failures_close);
  RUN_TEST(test_disconnect_is_quiet);
  RUN_TEST(test_validate_against_the_stream);
  return UNITY_END();
}
