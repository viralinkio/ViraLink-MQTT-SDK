// Client end to end over a fake transport: publish gating, PUBACK-driven
// delivery, replay after reconnect, timestamps, rate limiting, correlator and
// scheduler behaviour.
#include <string.h>
#include <unity.h>

#include <string>
#include <vector>

#include "Fakes.h"
#include "vl/Client.h"
#include "vl/core/Correlator.h"
#include "vl/core/Scheduler.h"

using namespace vl;
using vltest::FakeClock;
using vltest::FakeTransport;

void setUp() {}
void tearDown() {}

namespace {

class EchoModule : public IApiModule {
 public:
  Client* client = nullptr;
  int online = 0;
  std::vector<std::string> got;
  void attach(Client& c) override { client = &c; }
  void topics(TopicSink& s) override { s.add(InTopic::RpcRequest); }
  bool handle(const Topic& t, Bytes p) override {
    got.push_back(std::string(reinterpret_cast<const char*>(p.data()), p.size()));
    RecordMeta m;
    m.topic = OutTopic::RpcResponse;
    m.cls = RecordClass::Priority;
    m.arg = t.id;
    client->enqueue(m, reinterpret_cast<const uint8_t*>("{\"ok\":true}"), 11);
    return true;
  }
  void onOnline() override { ++online; }
};

struct Rig {
  FakeTransport t;
  FakeClock clock;
  RingOutbox<4096> outbox;
  Client c;
  EchoModule mod;
  Config cfg;
  Rig() : c(t, outbox) {
    c.setClock(clock);
    c.use(mod);
    cfg.host = "console.viralink.ir";
    cfg.auth = AccessToken("TOKEN123");
    cfg.ratePerSec = 0;
  }
  void connect() {
    TEST_ASSERT_EQUAL(Result::Ok, c.begin(cfg));
    c.loop();
    t.connack(0);
    t.subackAll();
    TEST_ASSERT_EQUAL(State::Online, c.state());
  }
};

}  // namespace

static void test_begin_validates_config() {
  Rig r;
  Config bad = r.cfg;
  bad.host = nullptr;
  TEST_ASSERT_EQUAL(Result::Rejected, r.c.begin(bad));
  bad = r.cfg;
  bad.clientId = "this-client-id-is-way-too-long";
  TEST_ASSERT_EQUAL(Result::Rejected, r.c.begin(bad));
  bad = r.cfg;
  bad.auth = X509();  // needs TLS
  TEST_ASSERT_EQUAL(Result::Rejected, r.c.begin(bad));
  bad = r.cfg;
  bad.auth = Credentials();
  TEST_ASSERT_EQUAL(Result::Rejected, r.c.begin(bad));
}

static void test_credentials_and_default_client_id() {
  Rig r;
  r.connect();
  TEST_ASSERT_EQUAL_STRING("TOKEN123", r.t.lastUser.c_str());
  TEST_ASSERT_NULL(r.t.lastOpts.password);
  TEST_ASSERT_EQUAL_STRING("console.viralink.ir", r.t.lastOpts.host);
  TEST_ASSERT_EQUAL_UINT(15, strlen(r.t.lastOpts.clientId));
  TEST_ASSERT_EQUAL_MEMORY("vl-", r.t.lastOpts.clientId, 3);
}

static void test_basic_auth_uses_its_client_id() {
  Rig r;
  r.cfg.auth = MqttBasic("dev-1", "user", "pass");
  r.connect();
  TEST_ASSERT_EQUAL_STRING("dev-1", r.t.lastOpts.clientId);
  TEST_ASSERT_EQUAL_STRING("user", r.t.lastOpts.username);
  TEST_ASSERT_EQUAL_STRING("pass", r.t.lastOpts.password);
}

static void test_nothing_published_before_online() {
  Rig r;
  TEST_ASSERT_EQUAL(Result::Queued, r.c.telemetry().add("t", 1).send());
  r.c.begin(r.cfg);
  r.c.loop();
  r.t.connack(0);
  r.c.loop();
  TEST_ASSERT_EQUAL(State::Subscribing, r.c.state());
  TEST_ASSERT_EQUAL_UINT(0, r.t.published.size());
  r.t.subackAll();
  TEST_ASSERT_EQUAL(1, r.mod.online);
  r.c.loop();
  TEST_ASSERT_EQUAL_UINT(1, r.t.published.size());
  TEST_ASSERT_EQUAL_STRING("v1/devices/me/telemetry", r.t.published[0].topic.c_str());
}

static void test_puback_frees_and_reconnect_replays_in_order() {
  Rig r;
  r.connect();
  r.c.telemetry().add("n", 1).send();
  r.c.telemetry().add("n", 2).send();
  r.c.attributes().add("fw", "1.0").send();
  r.c.loop();
  TEST_ASSERT_EQUAL_UINT(3, r.t.published.size());
  TEST_ASSERT_EQUAL_UINT(3, r.c.stats().inFlight);
  r.t.puback(r.t.published[0].packetId);
  TEST_ASSERT_EQUAL_UINT(2, r.c.stats().queuedRecords);

  // Link drops before the other PUBACKs: both are resent after reconnect.
  r.clock.advance(10000);
  r.t.close();
  TEST_ASSERT_EQUAL(State::Backoff, r.c.state());
  r.t.published.clear();
  r.clock.advance(2000);
  r.c.loop();
  TEST_ASSERT_EQUAL(State::Connecting, r.c.state());
  r.t.connack(0);
  r.t.subackAll();
  r.c.loop();
  TEST_ASSERT_EQUAL_UINT(2, r.t.published.size());
  TEST_ASSERT_EQUAL_STRING("{\"n\":2}", r.t.published[0].payload.c_str());
  TEST_ASSERT_EQUAL_STRING("v1/devices/me/attributes", r.t.published[1].topic.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"fw\":\"1.0\"}", r.t.published[1].payload.c_str());
  r.t.pubackAll();
  TEST_ASSERT_EQUAL_UINT(0, r.c.stats().queuedRecords);
}

static void test_max_in_flight() {
  Rig r;
  r.cfg.maxInFlight = 2;
  r.connect();
  for (int i = 0; i < 5; ++i) r.c.telemetry().add("i", i).send();
  r.c.loop();
  TEST_ASSERT_EQUAL_UINT(2, r.t.published.size());
  r.t.puback(r.t.published[0].packetId);
  r.c.loop();
  TEST_ASSERT_EQUAL_UINT(3, r.t.published.size());
}

static void test_failed_publish_retries_later() {
  Rig r;
  r.connect();
  r.c.telemetry().add("x", 1).send();
  r.t.failPublish = true;
  r.c.loop();
  TEST_ASSERT_EQUAL_UINT(0, r.t.published.size());
  r.t.failPublish = false;
  r.c.loop();
  TEST_ASSERT_EQUAL_UINT(1, r.t.published.size());
}

static void test_timestamps_from_utc_clock() {
  Rig r;
  r.clock.haveUtc = true;
  r.clock.utcAtZero = 1767225600000ull;
  r.connect();
  r.c.telemetry().add("t", 1).send();
  r.c.loop();
  char expect[64];
  snprintf(expect, sizeof(expect), "{\"ts\":%llu,\"values\":{\"t\":1}}",
           static_cast<unsigned long long>(1767225600000ull + r.clock.now));
  TEST_ASSERT_EQUAL_STRING(expect, r.t.published[0].payload.c_str());
}

static void test_offline_records_restamped_when_clock_syncs() {
  Rig r;
  r.clock.now = 5000;
  r.c.telemetry().add("t", 1).send();  // no wall clock yet
  r.clock.now = 65000;
  r.clock.haveUtc = true;
  r.clock.utcAtZero = 1767225600000ull;
  r.connect();
  r.c.loop();
  char expect[64];
  snprintf(expect, sizeof(expect), "{\"ts\":%llu,\"values\":{\"t\":1}}",
           static_cast<unsigned long long>(1767225600000ull + 5000));
  TEST_ASSERT_EQUAL_STRING(expect, r.t.published[0].payload.c_str());
}

static void test_without_clock_sent_without_ts() {
  Rig r;
  r.connect();
  r.c.telemetry().add("t", 1).send();
  r.c.loop();
  TEST_ASSERT_EQUAL_STRING("{\"t\":1}", r.t.published[0].payload.c_str());
}

static void test_explicit_ts() {
  Rig r;
  r.connect();
  r.c.telemetry().add("t", 1).ts(1767225600999ull).send();
  r.c.loop();
  TEST_ASSERT_EQUAL_STRING("{\"ts\":1767225600999,\"values\":{\"t\":1}}", r.t.published[0].payload.c_str());
}

static void test_builder_overflow_and_too_large() {
  Rig r;
  std::string big(300, 'x');
  TEST_ASSERT_EQUAL(Result::TooLarge, r.c.telemetry<64>().add("k", big.c_str()).send());
  r.t.packet = 256;
  r.connect();
  TEST_ASSERT_EQUAL_UINT(128, r.c.maxPayload());
  TEST_ASSERT_EQUAL(Result::TooLarge, r.c.telemetry<512>().add("k", big.c_str()).send());
  TEST_ASSERT_EQUAL(Result::Rejected, r.c.sendTelemetryJson("not json"));
  TEST_ASSERT_EQUAL(Result::Queued, r.c.sendTelemetryJson("{\"a\":1}"));
}

static void test_builder_copy_keeps_contents() {
  Rig r;
  r.connect();
  PayloadBuilder<64> b = r.c.telemetry<64>();
  b.add("a", 1);
  PayloadBuilder<64> copy(b);
  copy.add("b", 2).send();
  r.c.loop();
  TEST_ASSERT_EQUAL_STRING("{\"a\":1,\"b\":2}", r.t.published[0].payload.c_str());
}

static void test_incoming_message_routed_and_reply_jumps_queue() {
  Rig r;
  r.connect();
  r.cfg.maxInFlight = 4;
  for (int i = 0; i < 3; ++i) r.c.telemetry().add("i", i).send();
  r.t.message("v1/devices/me/rpc/request/17", "{\"method\":\"ping\"}");
  TEST_ASSERT_EQUAL_UINT(1, r.mod.got.size());
  r.c.loop();
  TEST_ASSERT_EQUAL_STRING("v1/devices/me/rpc/response/17", r.t.published[0].topic.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"ok\":true}", r.t.published[0].payload.c_str());
}

static void test_rate_limit() {
  Rig r;
  r.cfg.ratePerSec = 2;
  r.cfg.rateBurst = 2;
  r.connect();
  for (int i = 0; i < 6; ++i) r.c.telemetry().add("i", i).send();
  r.c.loop();
  TEST_ASSERT_EQUAL_UINT(2, r.t.published.size());
  r.t.pubackAll();
  r.c.loop();
  TEST_ASSERT_EQUAL_UINT(2, r.t.published.size());
  r.clock.advance(500);
  r.c.loop();
  TEST_ASSERT_EQUAL_UINT(3, r.t.published.size());
}

static void test_auth_failed_callback_and_new_credentials() {
  Rig r;
  static int fired = 0;
  fired = 0;
  r.c.onAuthFailed([](void*) { ++fired; }, nullptr);
  r.c.begin(r.cfg);
  r.t.connack(5);
  TEST_ASSERT_EQUAL(1, fired);
  TEST_ASSERT_EQUAL(State::AuthFailed, r.c.state());
  TEST_ASSERT_EQUAL(Result::Ok, r.c.setCredentials(AccessToken("NEWTOKEN")));
  TEST_ASSERT_EQUAL(State::Connecting, r.c.state());
  TEST_ASSERT_EQUAL_STRING("NEWTOKEN", r.t.lastUser.c_str());
}

static void test_state_callback() {
  Rig r;
  static std::vector<State> seen;
  seen.clear();
  r.c.onState([](State s, void*) { seen.push_back(s); }, nullptr);
  r.connect();
  TEST_ASSERT_EQUAL_UINT(3, seen.size());
  TEST_ASSERT_EQUAL(State::Connecting, seen[0]);
  TEST_ASSERT_EQUAL(State::Subscribing, seen[1]);
  TEST_ASSERT_EQUAL(State::Online, seen[2]);
}

static void test_pending_requests_resolve_or_time_out() {
  Rig r;
  r.connect();
  static std::vector<Result> results;
  results.clear();
  Correlator::Callback cb = [](Result res, const uint8_t*, size_t, void*) { results.push_back(res); };
  uint32_t a = r.c.addPending(1000, cb, nullptr);
  uint32_t b = r.c.addPending(5000, cb, nullptr);
  TEST_ASSERT_TRUE(a != 0 && b != 0 && a != b);
  TEST_ASSERT_TRUE(r.c.resolvePending(b, nullptr, 0));
  TEST_ASSERT_FALSE(r.c.resolvePending(b, nullptr, 0));
  r.clock.advance(1000);
  r.c.loop();
  TEST_ASSERT_EQUAL_UINT(2, results.size());
  TEST_ASSERT_EQUAL(Result::Ok, results[0]);
  TEST_ASSERT_EQUAL(Result::Timeout, results[1]);
  TEST_ASSERT_FALSE(r.c.resolvePending(a, nullptr, 0));
}

static void test_correlator_capacity() {
  Correlator c;
  for (size_t i = 0; i < Correlator::capacity(); ++i) TEST_ASSERT_TRUE(c.add(100, nullptr, nullptr) != 0);
  TEST_ASSERT_EQUAL_UINT32(0, c.add(100, nullptr, nullptr));
  Correlator::Callback cb;
  void* ctx;
  TEST_ASSERT_TRUE(c.takeExpired(100, cb, ctx));
  TEST_ASSERT_TRUE(c.add(100, nullptr, nullptr) != 0);
}

static void test_scheduler_drift_free_and_skips_missed_runs() {
  Scheduler s;
  Scheduler::Task fn = [](void*) {};
  TEST_ASSERT_EQUAL(Result::Ok, s.every(1000, fn, nullptr, 0));
  Scheduler::Task f;
  void* ctx;
  TEST_ASSERT_FALSE(s.takeDue(999, f, ctx));
  TEST_ASSERT_TRUE(s.takeDue(1003, f, ctx));  // late by 3 ms
  TEST_ASSERT_FALSE(s.takeDue(1999, f, ctx));
  TEST_ASSERT_TRUE(s.takeDue(2000, f, ctx));  // phase kept: due at 2000, not 2003
  // Stall of 5.5 intervals: one run, not five.
  TEST_ASSERT_TRUE(s.takeDue(7500, f, ctx));
  TEST_ASSERT_FALSE(s.takeDue(7999, f, ctx));
  TEST_ASSERT_TRUE(s.takeDue(8000, f, ctx));
  TEST_ASSERT_EQUAL(Result::Rejected, s.every(0, fn, nullptr, 0));
}

static void test_client_every_runs_on_loop() {
  Rig r;
  static int runs = 0;
  runs = 0;
  r.c.every(1000, [](void*) { ++runs; }, nullptr);
  r.c.loop();
  TEST_ASSERT_EQUAL(0, runs);
  r.clock.advance(1000);
  r.c.loop();
  TEST_ASSERT_EQUAL(1, runs);
}

static void test_end_fails_pending() {
  Rig r;
  r.connect();
  static Result last = Result::Ok;
  r.c.addPending(1000, [](Result res, const uint8_t*, size_t, void*) { last = res; }, nullptr);
  r.c.end();
  TEST_ASSERT_EQUAL(Result::NotReady, last);
  TEST_ASSERT_EQUAL(State::Idle, r.c.state());
}

static void test_cancel_pending_does_not_fire() {
  Rig r;
  r.connect();
  static int fired = 0;
  fired = 0;
  uint32_t id = r.c.addPending(1000, [](Result, const uint8_t*, size_t, void*) { ++fired; }, nullptr);
  TEST_ASSERT_TRUE(r.c.cancelPending(id));
  TEST_ASSERT_FALSE(r.c.cancelPending(id));
  r.clock.advance(2000);
  r.c.loop();
  TEST_ASSERT_EQUAL(0, fired);
}

static void test_claim() {
  Rig r;
  r.connect();
  TEST_ASSERT_EQUAL(Result::Queued, r.c.claim("s3cr3t", 60000));
  TEST_ASSERT_EQUAL(Result::Queued, r.c.claim(nullptr, 1000));
  r.c.loop();
  TEST_ASSERT_EQUAL_UINT(2, r.t.published.size());
  TEST_ASSERT_EQUAL_STRING("v1/devices/me/claim", r.t.published[0].topic.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"secretKey\":\"s3cr3t\",\"durationMs\":60000}", r.t.published[0].payload.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"durationMs\":1000}", r.t.published[1].payload.c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_begin_validates_config);
  RUN_TEST(test_credentials_and_default_client_id);
  RUN_TEST(test_basic_auth_uses_its_client_id);
  RUN_TEST(test_nothing_published_before_online);
  RUN_TEST(test_puback_frees_and_reconnect_replays_in_order);
  RUN_TEST(test_max_in_flight);
  RUN_TEST(test_failed_publish_retries_later);
  RUN_TEST(test_timestamps_from_utc_clock);
  RUN_TEST(test_offline_records_restamped_when_clock_syncs);
  RUN_TEST(test_without_clock_sent_without_ts);
  RUN_TEST(test_explicit_ts);
  RUN_TEST(test_builder_overflow_and_too_large);
  RUN_TEST(test_builder_copy_keeps_contents);
  RUN_TEST(test_incoming_message_routed_and_reply_jumps_queue);
  RUN_TEST(test_rate_limit);
  RUN_TEST(test_auth_failed_callback_and_new_credentials);
  RUN_TEST(test_state_callback);
  RUN_TEST(test_pending_requests_resolve_or_time_out);
  RUN_TEST(test_correlator_capacity);
  RUN_TEST(test_scheduler_drift_free_and_skips_missed_runs);
  RUN_TEST(test_client_every_runs_on_loop);
  RUN_TEST(test_end_fails_pending);
  RUN_TEST(test_cancel_pending_does_not_fire);
  RUN_TEST(test_claim);
  return UNITY_END();
}
