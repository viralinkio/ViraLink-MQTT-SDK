// Network manager with fake networks, transports and clock: choice of the
// best network, failover, fail-back hysteresis, stalled networks, shared
// transports, and a client that moves its session to another network.
#include <unity.h>

#include <string>
#include <vector>

#include "Fakes.h"
#include "vl/Client.h"
#include "vl/net/Networks.h"

using namespace vl;
using vltest::FakeClock;
using vltest::FakeTransport;

void setUp() {}
void tearDown() {}

namespace {

struct FakeNet : INetwork {
  const char* id;
  FakeTransport* t;
  bool isUp = false;
  int selects = 0;
  bool hasRssi = false;
  int16_t dBm = 0;

  FakeNet(const char* n, FakeTransport& tr) : id(n), t(&tr) {}
  const char* name() const override { return id; }
  bool up() override { return isUp; }
  IMqttTransport& transport() override { return *t; }
  void select() override { ++selects; }
  bool rssi(int16_t& out) override {
    out = dBm;
    return hasRssi;
  }
};

struct Events {
  std::vector<std::string> log;
  std::string take() {
    std::string r;
    for (size_t i = 0; i < log.size(); ++i) r += (i ? " | " : "") + log[i];
    log.clear();
    return r;
  }
};

void onConnack(uint8_t rc, void* ctx) { static_cast<Events*>(ctx)->log.push_back("connack " + std::to_string(rc)); }
void onSuback(const char* f, bool, void* ctx) { static_cast<Events*>(ctx)->log.push_back(std::string("suback ") + f); }
void onPuback(uint16_t id, void* ctx) { static_cast<Events*>(ctx)->log.push_back("puback " + std::to_string(id)); }
void onMessage(const char* topic, const uint8_t*, size_t, void* ctx) {
  static_cast<Events*>(ctx)->log.push_back(std::string("msg ") + topic);
}
void onClosed(void* ctx) { static_cast<Events*>(ctx)->log.push_back("closed"); }
void onChange(const char* from, const char* to, void* ctx) {
  static_cast<Events*>(ctx)->log.push_back(std::string("change ") + (from ? from : "none") + " -> " +
                                           (to ? to : "none"));
}

struct Rig {
  FakeClock clock;
  FakeTransport wifiT, ethT, gsmT;
  FakeNet wifi, eth, gsm;
  Networks<3> nets;
  Events ev;
  ConnectOptions o;

  Rig() : wifi("wifi", wifiT), eth("eth", ethT), gsm("gsm", gsmT), nets(&clock) {
    TEST_ASSERT_EQUAL(Result::Ok, nets.add(wifi));
    TEST_ASSERT_EQUAL(Result::Ok, nets.add(eth));
    TEST_ASSERT_EQUAL(Result::Ok, nets.add(gsm));
    nets.onConnack(onConnack, &ev);
    nets.onSuback(onSuback, &ev);
    nets.onPuback(onPuback, &ev);
    nets.onMessage(onMessage, &ev);
    nets.onClosed(onClosed, &ev);
    o.host = "broker";
  }
  const char* active() { return nets.active() ? nets.active()->name() : "none"; }
  // The session's view: connect, and get a CONNACK through the active network.
  void online() {
    nets.beginConnect(o);
    static_cast<FakeNet*>(nets.active())->t->connack(0);
    TEST_ASSERT_EQUAL_STRING("connack 0", ev.take().c_str());
  }
};

}  // namespace

static void test_connects_through_the_best_network_up() {
  Rig r;
  r.eth.isUp = r.gsm.isUp = true;
  r.nets.loop();
  TEST_ASSERT_TRUE(r.nets.linkUp());
  TEST_ASSERT_EQUAL_STRING("eth", r.active());
  TEST_ASSERT_EQUAL(1, r.eth.selects);
  r.nets.beginConnect(r.o);
  TEST_ASSERT_EQUAL(2, r.eth.selects);  // again before every connect
  TEST_ASSERT_EQUAL(0, r.wifiT.connectCalls);
  TEST_ASSERT_EQUAL(1, r.ethT.connectCalls);
  r.ethT.connack(0);
  TEST_ASSERT_TRUE(r.nets.connected());
  uint16_t id = 0;
  TEST_ASSERT_TRUE(r.nets.publish("t", reinterpret_cast<const uint8_t*>("x"), 1, 1, &id));
  TEST_ASSERT_EQUAL_UINT(1, r.ethT.published.size());
  r.nets.subscribe("v1/devices/me/rpc/request/+", 1);
  TEST_ASSERT_EQUAL_UINT(1, r.ethT.subscribed.size());
  r.ethT.subackAll();
  r.ethT.puback(id);
  r.ethT.message("v1/devices/me/attributes", "{}");
  TEST_ASSERT_EQUAL_STRING("connack 0 | suback v1/devices/me/rpc/request/+ | puback 1 | msg v1/devices/me/attributes",
                           r.ev.take().c_str());
}

static void test_no_network_up() {
  Rig r;
  r.nets.beginConnect(r.o);  // before any loop(): looks at the networks itself
  TEST_ASSERT_FALSE(r.nets.linkUp());
  TEST_ASSERT_FALSE(r.nets.connected());
  TEST_ASSERT_EQUAL(0, r.wifiT.connectCalls + r.ethT.connectCalls + r.gsmT.connectCalls);
  TEST_ASSERT_FALSE(r.nets.publish("t", nullptr, 0, 1, nullptr));
  TEST_ASSERT_EQUAL_STRING("", r.ev.take().c_str());  // never from inside beginConnect
  r.nets.loop();
  TEST_ASSERT_EQUAL_STRING("closed", r.ev.take().c_str());
  r.nets.loop();
  TEST_ASSERT_EQUAL_STRING("", r.ev.take().c_str());

  r.wifi.isUp = true;
  r.nets.loop();
  TEST_ASSERT_TRUE(r.nets.linkUp());
  TEST_ASSERT_EQUAL_STRING("wifi", r.active());
}

static void test_failover_at_once() {
  Rig r;
  r.nets.onChange(onChange, &r.ev);
  r.wifi.isUp = r.eth.isUp = true;
  r.nets.loop();
  TEST_ASSERT_EQUAL_STRING("change none -> wifi", r.ev.take().c_str());
  r.online();

  r.wifi.isUp = false;
  r.nets.loop();
  TEST_ASSERT_EQUAL_STRING("change wifi -> eth | closed", r.ev.take().c_str());
  TEST_ASSERT_EQUAL(1, r.wifiT.disconnectCalls);
  TEST_ASSERT_EQUAL_STRING("eth", r.active());
  TEST_ASSERT_EQUAL(1, r.eth.selects);
  TEST_ASSERT_FALSE(r.nets.connected());
  r.nets.loop();
  TEST_ASSERT_EQUAL_STRING("", r.ev.take().c_str());

  r.nets.disconnect();  // the session failing, then retrying
  r.nets.beginConnect(r.o);
  TEST_ASSERT_EQUAL(1, r.ethT.connectCalls);

  // Last network gone: closed, and no path at all.
  r.ethT.connack(0);
  r.ev.take();
  r.eth.isUp = false;
  r.nets.loop();
  TEST_ASSERT_EQUAL_STRING("change eth -> none | closed", r.ev.take().c_str());
  TEST_ASSERT_FALSE(r.nets.linkUp());
}

static void test_fail_back_after_30s_up_without_a_break() {
  Rig r;
  r.eth.isUp = true;
  r.nets.loop();
  r.online();
  TEST_ASSERT_EQUAL_STRING("eth", r.active());

  r.wifi.isUp = true;
  r.nets.loop();
  r.clock.advance(20000);
  r.nets.loop();
  r.wifi.isUp = false;  // flap: the 30 s start again
  r.nets.loop();
  r.wifi.isUp = true;
  r.nets.loop();
  r.clock.advance(29999);
  r.nets.loop();
  TEST_ASSERT_EQUAL_STRING("eth", r.active());
  TEST_ASSERT_EQUAL_STRING("", r.ev.take().c_str());
  r.clock.advance(1);
  r.nets.loop();
  TEST_ASSERT_EQUAL_STRING("wifi", r.active());
  TEST_ASSERT_EQUAL_STRING("closed", r.ev.take().c_str());
  TEST_ASSERT_EQUAL(1, r.ethT.disconnectCalls);
  TEST_ASSERT_EQUAL(1, r.wifi.selects);
}

static void test_move_without_a_connection_is_silent() {
  Rig r;
  r.gsm.isUp = true;
  r.nets.loop();
  r.online();
  r.nets.disconnect();  // session in backoff
  TEST_ASSERT_EQUAL(1, r.gsmT.disconnectCalls);
  r.eth.isUp = true;
  r.nets.loop();
  r.clock.advance(30000);
  r.nets.loop();
  TEST_ASSERT_EQUAL_STRING("eth", r.active());
  TEST_ASSERT_EQUAL_STRING("", r.ev.take().c_str());
  TEST_ASSERT_EQUAL(1, r.gsmT.disconnectCalls);
}

static void test_stalled_network_is_left_and_sits_out() {
  Rig r;
  r.wifi.isUp = r.eth.isUp = true;
  r.nets.loop();
  // WiFi is associated but reaches nothing: the session keeps retrying.
  for (int i = 0; i < 4; ++i) {
    r.nets.beginConnect(r.o);
    r.clock.advance(30000);
    r.nets.loop();
    if (i < 3) {
      TEST_ASSERT_EQUAL_STRING("wifi", r.active());
      r.nets.disconnect();
    }
  }
  TEST_ASSERT_EQUAL_STRING("eth", r.active());  // 120 s without a CONNACK
  TEST_ASSERT_EQUAL_STRING("closed", r.ev.take().c_str());
  r.online();

  // WiFi stays up, but may not win the session back for 5 minutes.
  r.clock.advance(299999);
  r.nets.loop();
  TEST_ASSERT_EQUAL_STRING("eth", r.active());
  r.clock.advance(1);
  r.nets.loop();
  TEST_ASSERT_EQUAL_STRING("wifi", r.active());
}

static void test_penalised_network_taken_when_the_others_go() {
  Rig r;
  r.wifi.isUp = r.eth.isUp = true;
  r.nets.loop();
  r.nets.beginConnect(r.o);
  for (int i = 0; i < 4; ++i) {
    r.clock.advance(30000);
    r.nets.loop();
    if (i < 3) {
      r.nets.disconnect();
      r.nets.beginConnect(r.o);
    }
  }
  TEST_ASSERT_EQUAL_STRING("eth", r.active());  // WiFi stalled and sits out
  r.eth.isUp = false;
  r.nets.loop();
  TEST_ASSERT_EQUAL_STRING("wifi", r.active());  // still better than nothing
}

static void test_stalled_network_still_used_when_alone() {
  Rig r;
  r.wifi.isUp = true;
  r.nets.loop();
  for (int i = 0; i < 6; ++i) {
    r.nets.beginConnect(r.o);
    r.clock.advance(30000);
    r.nets.loop();
    r.nets.disconnect();
  }
  TEST_ASSERT_EQUAL_STRING("wifi", r.active());
  TEST_ASSERT_EQUAL_STRING("", r.ev.take().c_str());
}

static void test_any_connack_restarts_the_stall_clock() {
  Rig r;
  r.wifi.isUp = r.eth.isUp = true;
  r.nets.loop();
  r.nets.beginConnect(r.o);
  r.clock.advance(100000);
  r.wifiT.connack(4);  // credentials refused, but the network works
  r.ev.take();
  for (int i = 0; i < 3; ++i) {  // 190 s since the first attempt, 90 s since the CONNACK
    r.nets.disconnect();
    r.nets.beginConnect(r.o);
    r.clock.advance(30000);
    r.nets.loop();
  }
  TEST_ASSERT_EQUAL_STRING("wifi", r.active());
}

static void test_stopped_session_is_no_stall() {
  Rig r;
  r.wifi.isUp = r.eth.isUp = true;
  r.nets.loop();
  r.nets.beginConnect(r.o);
  r.nets.disconnect();  // Client::stop()
  r.clock.advance(600000);
  r.nets.loop();
  TEST_ASSERT_EQUAL_STRING("wifi", r.active());
}

static void test_events_from_other_networks_ignored() {
  Rig r;
  r.wifi.isUp = r.eth.isUp = true;
  r.nets.loop();
  r.online();
  r.wifi.isUp = false;
  r.nets.loop();
  r.ev.take();
  r.wifiT.close();
  r.wifiT.puback(7);
  r.wifiT.connack(0);
  r.wifiT.message("t", "x");
  TEST_ASSERT_EQUAL_STRING("", r.ev.take().c_str());
  TEST_ASSERT_FALSE(r.nets.connected());
}

static void test_closed_by_the_active_transport() {
  Rig r;
  r.eth.isUp = true;
  r.nets.loop();
  r.online();
  r.ethT.close();
  TEST_ASSERT_EQUAL_STRING("closed", r.ev.take().c_str());
  r.nets.loop();  // the transport already reported it
  TEST_ASSERT_EQUAL_STRING("", r.ev.take().c_str());
}

static void test_shared_transport() {
  FakeClock clock;
  FakeTransport t;
  FakeNet wifi("wifi", t), eth("eth", t);
  Networks<2> nets(&clock);
  nets.add(wifi);
  nets.add(eth);
  Events ev;
  nets.onConnack(onConnack, &ev);
  nets.onClosed(onClosed, &ev);
  ConnectOptions o;
  t.packet = 700;
  TEST_ASSERT_EQUAL_UINT(700, nets.maxPacket());

  eth.isUp = true;
  nets.loop();
  nets.beginConnect(o);
  t.connack(0);
  TEST_ASSERT_EQUAL_STRING("connack 0", ev.take().c_str());
  wifi.isUp = true;
  nets.loop();
  clock.advance(30000);
  nets.loop();
  TEST_ASSERT_EQUAL_STRING("closed", ev.take().c_str());
  TEST_ASSERT_EQUAL(1, t.disconnectCalls);
  TEST_ASSERT_EQUAL(1, wifi.selects);
  nets.disconnect();
  nets.beginConnect(o);
  TEST_ASSERT_EQUAL(2, t.connectCalls);
  t.connack(0);  // registered once per transport, still reaches the client
  TEST_ASSERT_EQUAL_STRING("connack 0", ev.take().c_str());
}

static void test_validate_and_max_packet() {
  Rig r;
  r.wifiT.packet = 4096;
  r.ethT.packet = 1024;
  r.gsmT.packet = 2048;
  TEST_ASSERT_EQUAL_UINT(1024, r.nets.maxPacket());
  TEST_ASSERT_EQUAL(Result::Ok, r.nets.validate(r.o));
  r.gsmT.verdict = Result::Rejected;  // e.g. TLS over a plain modem stream
  TEST_ASSERT_EQUAL(Result::Rejected, r.nets.validate(r.o));

  Networks<1> none;
  TEST_ASSERT_EQUAL(Result::Rejected, none.validate(r.o));
  TEST_ASSERT_EQUAL_UINT(0, none.maxPacket());
}

static void test_capacity() {
  FakeTransport t;
  FakeNet a("a", t), b("b", t);
  Networks<1> nets;
  TEST_ASSERT_EQUAL(Result::Ok, nets.add(a));
  TEST_ASSERT_EQUAL(Result::NoMemory, nets.add(b));
}

static void test_system_info_names_the_active_network() {
  Rig r;
  int16_t dBm = 0;
  TEST_ASSERT_NULL(r.nets.iface());
  TEST_ASSERT_FALSE(r.nets.rssi(dBm));
  r.gsm.isUp = true;
  r.gsm.hasRssi = true;
  r.gsm.dBm = -87;
  r.nets.loop();
  TEST_ASSERT_EQUAL_STRING("gsm", r.nets.iface());
  TEST_ASSERT_TRUE(r.nets.rssi(dBm));
  TEST_ASSERT_EQUAL_INT16(-87, dBm);
}

static void test_link_network() {
  FakeTransport t;
  static bool modemUp = false;
  LinkNetwork gsm("gsm", t, [](void*) { return modemUp; });
  TEST_ASSERT_EQUAL_STRING("gsm", gsm.name());
  TEST_ASSERT_TRUE(&gsm.transport() == &t);
  TEST_ASSERT_FALSE(gsm.up());
  modemUp = true;
  TEST_ASSERT_TRUE(gsm.up());

  // A slow check (an AT command) runs at most every checkMs.
  static int checks = 0;
  FakeClock clock;
  LinkNetwork slow("gsm", t, [](void*) { ++checks; return modemUp; }, nullptr, 2000, &clock);
  TEST_ASSERT_TRUE(slow.up());
  modemUp = false;
  clock.advance(1999);
  TEST_ASSERT_TRUE(slow.up());
  TEST_ASSERT_EQUAL(1, checks);
  clock.advance(1);
  TEST_ASSERT_FALSE(slow.up());
  TEST_ASSERT_EQUAL(2, checks);
}

// The whole client: a record sent on WiFi but not acknowledged goes out again
// over Ethernet when WiFi drops.
static void test_client_moves_its_session() {
  FakeClock clock;
  FakeTransport wifiT, ethT;
  FakeNet wifi("wifi", wifiT), eth("eth", ethT);
  Networks<2> nets(&clock);
  nets.add(wifi);
  nets.add(eth);
  RingOutbox<4096> outbox;
  Client c(nets, outbox);
  c.setClock(clock);
  Config cfg;
  cfg.host = "console.viralink.ir";
  cfg.auth = AccessToken("TOKEN");
  cfg.ratePerSec = 0;

  wifi.isUp = eth.isUp = true;
  TEST_ASSERT_EQUAL(Result::Ok, c.begin(cfg));
  c.loop();
  wifiT.connack(0);
  wifiT.subackAll();
  TEST_ASSERT_EQUAL(State::Online, c.state());
  TEST_ASSERT_TRUE(ok(c.telemetry().add("temp", 21).send()));
  c.loop();
  TEST_ASSERT_EQUAL_UINT(1, wifiT.published.size());

  wifi.isUp = false;
  c.loop();
  TEST_ASSERT_EQUAL(State::Backoff, c.state());
  clock.advance(2000);
  c.loop();
  TEST_ASSERT_EQUAL(State::Connecting, c.state());
  TEST_ASSERT_EQUAL(1, ethT.connectCalls);
  ethT.connack(0);
  ethT.subackAll();
  TEST_ASSERT_EQUAL(State::Online, c.state());
  c.loop();
  TEST_ASSERT_EQUAL_UINT(1, ethT.published.size());
  TEST_ASSERT_EQUAL_STRING(wifiT.published[0].payload.c_str(), ethT.published[0].payload.c_str());
}

// No network at begin(): the client connects the moment one comes up rather
// than waiting out its backoff.
static void test_client_connects_when_a_network_appears() {
  FakeClock clock;
  FakeTransport wifiT;
  FakeNet wifi("wifi", wifiT);
  Networks<1> nets(&clock);
  nets.add(wifi);
  RingOutbox<1024> outbox;
  Client c(nets, outbox);
  c.setClock(clock);
  Config cfg;
  cfg.host = "console.viralink.ir";
  cfg.auth = AccessToken("TOKEN");
  TEST_ASSERT_EQUAL(Result::Ok, c.begin(cfg));
  for (int i = 0; i < 20; ++i) {  // a long outage: the backoff grows to its cap
    clock.advance(30000);
    c.loop();
  }
  c.loop();  // report the last failed attempt
  TEST_ASSERT_EQUAL(State::Backoff, c.state());
  TEST_ASSERT_EQUAL(0, wifiT.connectCalls);
  wifi.isUp = true;
  clock.advance(10);
  c.loop();
  c.loop();
  TEST_ASSERT_EQUAL(State::Connecting, c.state());
  TEST_ASSERT_EQUAL(1, wifiT.connectCalls);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_connects_through_the_best_network_up);
  RUN_TEST(test_no_network_up);
  RUN_TEST(test_failover_at_once);
  RUN_TEST(test_fail_back_after_30s_up_without_a_break);
  RUN_TEST(test_move_without_a_connection_is_silent);
  RUN_TEST(test_stalled_network_is_left_and_sits_out);
  RUN_TEST(test_penalised_network_taken_when_the_others_go);
  RUN_TEST(test_stalled_network_still_used_when_alone);
  RUN_TEST(test_any_connack_restarts_the_stall_clock);
  RUN_TEST(test_stopped_session_is_no_stall);
  RUN_TEST(test_events_from_other_networks_ignored);
  RUN_TEST(test_closed_by_the_active_transport);
  RUN_TEST(test_shared_transport);
  RUN_TEST(test_validate_and_max_packet);
  RUN_TEST(test_capacity);
  RUN_TEST(test_system_info_names_the_active_network);
  RUN_TEST(test_link_network);
  RUN_TEST(test_client_moves_its_session);
  RUN_TEST(test_client_connects_when_a_network_appears);
  return UNITY_END();
}
