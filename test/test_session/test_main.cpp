// Session state machine driven by a fake transport and a fake clock,
// including every CONNACK return code.
#include <unity.h>

#include <vector>

#include "Fakes.h"
#include "vl/core/Router.h"
#include "vl/core/Session.h"

using namespace vl;
using vltest::FakeTransport;

void setUp() {}
void tearDown() {}

namespace {

class TwoTopicModule : public IApiModule {
 public:
  void topics(TopicSink& s) override {
    s.add(InTopic::RpcRequest);
    s.add(InTopic::AttrUpdate);
  }
  bool handle(const Topic&, Bytes) override { return true; }
};

class Recorder : public SessionListener {
 public:
  std::vector<State> states;
  int authFailed = 0;
  uint8_t lastRc = 0;
  int linkLost = 0;
  void onSessionState(State, State to) override { states.push_back(to); }
  void onSessionAuthFailed(uint8_t rc) override {
    ++authFailed;
    lastRc = rc;
  }
  void onSessionLinkLost() override { ++linkLost; }
};

struct Rig {
  FakeTransport t;
  Router router;
  TwoTopicModule mod;
  Session s;
  Recorder rec;
  ConnectOptions opts;
  Rig() : s(t, router) {
    router.add(mod);
    s.setListener(&rec);
    s.seed(42);
    opts.host = "broker";
    opts.clientId = "vl-test";
  }
  void startAndConnect(uint64_t now) {
    s.start(opts, now);
    t.connack(0);
    s.connack(0, now);
    for (size_t i = 0; i < 2; ++i) s.suback(router.filter(i), true, now);
  }
};

}  // namespace

static void test_online_only_after_every_suback() {
  Rig r;
  r.s.start(r.opts, 0);
  TEST_ASSERT_EQUAL(State::Connecting, r.s.state());
  TEST_ASSERT_EQUAL(1, r.t.connectCalls);
  r.t.isConnected = true;
  r.s.connack(0, 10);
  TEST_ASSERT_EQUAL(State::Subscribing, r.s.state());
  TEST_ASSERT_EQUAL_UINT(2, r.t.subscribed.size());
  r.s.suback(r.router.filter(0), true, 20);
  TEST_ASSERT_EQUAL(State::Subscribing, r.s.state());
  r.s.suback("unknown/filter", true, 20);
  TEST_ASSERT_EQUAL(State::Subscribing, r.s.state());
  r.s.suback(r.router.filter(1), true, 30);
  TEST_ASSERT_EQUAL(State::Online, r.s.state());
}

static void test_resubscribes_on_every_reconnect() {
  Rig r;
  r.startAndConnect(0);
  r.t.subscribed.clear();
  r.s.closed(100);
  TEST_ASSERT_EQUAL(State::Backoff, r.s.state());
  TEST_ASSERT_EQUAL(1, r.rec.linkLost);
  r.s.tick(r.s.retryAt());
  TEST_ASSERT_EQUAL(State::Connecting, r.s.state());
  r.t.isConnected = true;
  r.s.connack(0, r.s.retryAt());
  TEST_ASSERT_EQUAL_UINT(2, r.t.subscribed.size());
}

static void test_refused_suback_fails_session() {
  Rig r;
  r.s.start(r.opts, 0);
  r.t.isConnected = true;
  r.s.connack(0, 0);
  r.s.suback(r.router.filter(0), false, 0);
  TEST_ASSERT_EQUAL(State::Backoff, r.s.state());
}

static void test_suback_timeout() {
  Rig r;
  r.s.start(r.opts, 0);
  r.t.isConnected = true;
  r.s.connack(0, 0);
  r.s.tick(9999);
  TEST_ASSERT_EQUAL(State::Subscribing, r.s.state());
  r.s.tick(10000);
  TEST_ASSERT_EQUAL(State::Backoff, r.s.state());
}

static void test_connect_timeout() {
  Rig r;
  r.s.start(r.opts, 0);
  r.s.tick(29999);
  TEST_ASSERT_EQUAL(State::Connecting, r.s.state());
  r.s.tick(30000);
  TEST_ASSERT_EQUAL(State::Backoff, r.s.state());
  TEST_ASSERT_EQUAL(0, r.rec.linkLost);  // never had a link
}

static void test_auth_failure_codes() {
  const uint8_t codes[] = {2, 4, 5};
  for (size_t i = 0; i < 3; ++i) {
    Rig r;
    r.s.start(r.opts, 0);
    r.s.connack(codes[i], 0);
    TEST_ASSERT_EQUAL(State::AuthFailed, r.s.state());
    TEST_ASSERT_EQUAL(1, r.rec.authFailed);
    TEST_ASSERT_EQUAL_UINT8(codes[i], r.rec.lastRc);
    // Slow retry: nothing for 5 minutes, then a new attempt.
    r.s.tick(299999);
    TEST_ASSERT_EQUAL(State::AuthFailed, r.s.state());
    r.s.tick(300000);
    TEST_ASSERT_EQUAL(State::Connecting, r.s.state());
  }
}

static void test_auth_failed_reconnect_now() {
  Rig r;
  r.s.start(r.opts, 0);
  r.s.connack(4, 0);
  r.s.reconnectNow(5);
  TEST_ASSERT_EQUAL(State::Connecting, r.s.state());
  TEST_ASSERT_EQUAL(2, r.t.connectCalls);
}

static void test_server_unavailable_waits_at_least_30s() {
  Rig r;
  r.s.start(r.opts, 0);
  r.s.connack(3, 0);
  TEST_ASSERT_EQUAL(State::Backoff, r.s.state());
  TEST_ASSERT_TRUE(r.s.retryAt() >= 30000);
}

static void test_other_codes_back_off() {
  Rig r;
  r.s.start(r.opts, 0);
  r.s.connack(1, 0);  // unacceptable protocol version
  TEST_ASSERT_EQUAL(State::Backoff, r.s.state());
  Rig r2;
  r2.s.start(r2.opts, 0);
  r2.s.connack(77, 0);
  TEST_ASSERT_EQUAL(State::Backoff, r2.s.state());
}

static void test_backoff_grows_with_jitter_and_caps() {
  Rig r;
  r.s.start(r.opts, 0);
  uint64_t now = 0;
  uint64_t prev = 0;
  for (int i = 0; i < 12; ++i) {
    r.s.closed(now);  // Connecting -> Backoff
    uint64_t delay = r.s.retryAt() - now;
    uint64_t nominal = i >= 6 ? 60000 : (1000ull << i);
    TEST_ASSERT_TRUE(delay >= nominal * 80 / 100);
    TEST_ASSERT_TRUE(delay <= nominal * 120 / 100);
    if (i > 0 && i < 6) TEST_ASSERT_TRUE(delay > prev);
    prev = delay;
    now = r.s.retryAt();
    r.s.tick(now);
    TEST_ASSERT_EQUAL(State::Connecting, r.s.state());
  }
}

static void test_backoff_resets_after_online() {
  Rig r;
  r.s.start(r.opts, 0);
  for (int i = 0; i < 5; ++i) {
    r.s.closed(0);
    r.s.tick(r.s.retryAt());
  }
  r.t.isConnected = true;
  r.s.connack(0, 0);
  r.s.suback(r.router.filter(0), true, 0);
  r.s.suback(r.router.filter(1), true, 0);
  TEST_ASSERT_EQUAL(State::Online, r.s.state());
  r.s.closed(1000);
  TEST_ASSERT_TRUE(r.s.retryAt() - 1000 <= 1200);
}

static void test_online_detects_lost_link_without_closed_event() {
  Rig r;
  r.startAndConnect(0);
  TEST_ASSERT_EQUAL(State::Online, r.s.state());
  r.t.isConnected = false;
  r.s.tick(5);
  TEST_ASSERT_EQUAL(State::Backoff, r.s.state());
  TEST_ASSERT_EQUAL(1, r.rec.linkLost);
}

static void test_late_events_are_ignored() {
  Rig r;
  r.s.start(r.opts, 0);
  r.s.connack(4, 0);
  r.s.closed(1);  // transport closes after refusing
  TEST_ASSERT_EQUAL(State::AuthFailed, r.s.state());
  r.s.connack(0, 2);  // stale CONNACK
  TEST_ASSERT_EQUAL(State::AuthFailed, r.s.state());
}

static void test_no_modules_goes_online_on_connack() {
  FakeTransport t;
  Router router;
  Session s(t, router);
  ConnectOptions o;
  o.host = "h";
  s.start(o, 0);
  t.isConnected = true;
  s.connack(0, 0);
  TEST_ASSERT_EQUAL(State::Online, s.state());
}

namespace {
class Counter : public IApiModule {
 public:
  int seen = 0;
  void topics(TopicSink& s) override {
    s.add(InTopic::AttrUpdate);
    s.add(InTopic::RpcRequest);
  }
  bool handle(const Topic&, Bytes) override {
    ++seen;
    return true;
  }
};
}  // namespace

// Attributes and OTA both need shared-attribute updates; RPC requests still
// go to one consumer only.
static void test_attribute_updates_reach_every_subscriber() {
  Router router;
  Counter a, b;
  router.add(a);
  router.add(b);
  TEST_ASSERT_TRUE(router.dispatch("v1/devices/me/attributes", Bytes()));
  TEST_ASSERT_EQUAL(1, a.seen);
  TEST_ASSERT_EQUAL(1, b.seen);
  TEST_ASSERT_TRUE(router.dispatch("v1/devices/me/rpc/request/1", Bytes()));
  TEST_ASSERT_EQUAL(2, a.seen);
  TEST_ASSERT_EQUAL(1, b.seen);
}

static void test_stop() {
  Rig r;
  r.startAndConnect(0);
  r.s.stop();
  TEST_ASSERT_EQUAL(State::Idle, r.s.state());
  TEST_ASSERT_EQUAL(1, r.rec.linkLost);
  r.s.tick(1000000);
  TEST_ASSERT_EQUAL(State::Idle, r.s.state());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_online_only_after_every_suback);
  RUN_TEST(test_resubscribes_on_every_reconnect);
  RUN_TEST(test_refused_suback_fails_session);
  RUN_TEST(test_suback_timeout);
  RUN_TEST(test_connect_timeout);
  RUN_TEST(test_auth_failure_codes);
  RUN_TEST(test_auth_failed_reconnect_now);
  RUN_TEST(test_server_unavailable_waits_at_least_30s);
  RUN_TEST(test_other_codes_back_off);
  RUN_TEST(test_backoff_grows_with_jitter_and_caps);
  RUN_TEST(test_backoff_resets_after_online);
  RUN_TEST(test_online_detects_lost_link_without_closed_event);
  RUN_TEST(test_late_events_are_ignored);
  RUN_TEST(test_no_modules_goes_online_on_connack);
  RUN_TEST(test_attribute_updates_reach_every_subscriber);
  RUN_TEST(test_stop);
  return UNITY_END();
}
