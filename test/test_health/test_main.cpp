// Health attributes: a full report on every connect, changing keys on the
// interval, nothing offline or during provisioning, switchable keys and the
// 1.0.2 names.
#include <unity.h>

#include <string>
#include <vector>

#include "ClientRig.h"
#include "vl/api/Health.h"
#include "vl/api/Provision.h"

using namespace vl;
using vltest::ClientRig;
using vltest::FakeSystemInfo;
using vltest::Published;

void setUp() {}
void tearDown() {}

namespace {

const char* const kSdk = "\"vl_sdk\":\"" VL_SDK_VERSION "\"";

struct HealthRig : ClientRig {
  FakeSystemInfo info;
  Health health;

  explicit HealthRig(uint32_t intervalMs = 60000) : health(intervalMs) {
    health.setSource(info);
    c.use(health);
    clock.now = 3600500;  // vl_uptime 3600
  }
  // The one record flush() published, which must be a health report.
  std::string report() {
    std::vector<Published> out = flush();
    TEST_ASSERT_EQUAL_UINT(1, out.size());
    TEST_ASSERT_EQUAL_STRING("v1/devices/me/attributes", out[0].topic.c_str());
    return out[0].payload;
  }
  void assertNothing() { TEST_ASSERT_EQUAL_UINT(0, flush().size()); }
  void reconnect() {
    t.close();
    clock.advance(2000);
    c.loop();
    t.connack(0);
    t.subackAll();
    TEST_ASSERT_EQUAL(State::Online, c.state());
  }
};

}  // namespace

static void test_full_report_on_online() {
  HealthRig r;
  r.connect();
  std::string expected =
      "{\"vl_uptime\":3600,\"vl_heap_free\":180000,\"vl_heap_min\":150000,\"vl_reset_reason\":\"power_on\","
      "\"vl_rssi\":-61,\"vl_iface\":\"wifi\",\"vl_queue_depth\":0,\"vl_dropped\":0,\"vl_dead_letter\":0," +
      std::string(kSdk) + "}";
  TEST_ASSERT_EQUAL_STRING(expected.c_str(), r.report().c_str());
}

static void test_interval_reports_only_changing_keys() {
  HealthRig r;
  r.connect();
  r.report();
  r.clock.advance(59999);
  r.assertNothing();
  r.clock.advance(1);
  r.info.free = 170000;
  TEST_ASSERT_EQUAL_STRING(
      "{\"vl_uptime\":3660,\"vl_heap_free\":170000,\"vl_heap_min\":150000,\"vl_rssi\":-61,\"vl_iface\":\"wifi\","
      "\"vl_queue_depth\":0,\"vl_dropped\":0,\"vl_dead_letter\":0}",
      r.report().c_str());
  r.clock.advance(60000);
  r.report();
}

static void test_interval_zero_reports_only_on_connect() {
  HealthRig r(0);
  r.connect();
  r.report();
  r.clock.advance(3600000);
  r.assertNothing();
}

static void test_nothing_while_offline() {
  HealthRig r;
  TEST_ASSERT_EQUAL(Result::Ok, r.c.begin(r.cfg));
  r.c.loop();
  r.clock.advance(120000);
  r.c.loop();
  TEST_ASSERT_EQUAL_UINT(0, r.c.stats().queuedRecords);

  r.connect();
  r.report();
  r.t.close();
  r.clock.advance(120000);
  r.c.loop();  // reconnecting: no CONNACK yet
  TEST_ASSERT_NOT_EQUAL(State::Online, r.c.state());
  TEST_ASSERT_EQUAL_UINT(0, r.c.stats().queuedRecords);
}

static void test_reconnect_sends_full_report_and_restarts_timer() {
  HealthRig r;
  r.connect();
  r.report();
  r.clock.advance(30000);
  r.reconnect();
  TEST_ASSERT_NOT_NULL(strstr(r.report().c_str(), kSdk));
  r.clock.advance(59999);
  r.assertNothing();
  r.clock.advance(1);
  TEST_ASSERT_NULL(strstr(r.report().c_str(), kSdk));
}

static void test_keys_switched_off() {
  HealthRig r;
  r.health.setKeys(kHealthUptime | kHealthRssi | kHealthSdk);
  r.connect();
  std::string expected = "{\"vl_uptime\":3600,\"vl_rssi\":-61," + std::string(kSdk) + "}";
  TEST_ASSERT_EQUAL_STRING(expected.c_str(), r.report().c_str());

  // An interval with no changing key enabled sends nothing.
  r.health.setKeys(kHealthSdk | kHealthResetReason);
  r.clock.advance(60000);
  r.assertNothing();
  r.health.setKeys(0);
  r.reconnect();
  r.assertNothing();
}

static void test_v1_key_names() {
  HealthRig r;
  r.health.useV1Keys(true);
  r.health.setKeys(kHealthUptime | kHealthHeapFree | kHealthSdk);
  r.connect();
  std::string expected = "{\"upTime\":3600,\"ESP Free Heap\":180000,\"Viralink SDK Version\":\"" VL_SDK_VERSION "\"}";
  TEST_ASSERT_EQUAL_STRING(expected.c_str(), r.report().c_str());
}

static void test_unknown_platform_values_omitted() {
  HealthRig r;
  r.info.hasHeap = false;
  r.info.hasRssi = false;
  r.info.reason = nullptr;
  r.info.link = nullptr;
  r.connect();
  std::string expected =
      "{\"vl_uptime\":3600,\"vl_queue_depth\":0,\"vl_dropped\":0,\"vl_dead_letter\":0," + std::string(kSdk) + "}";
  TEST_ASSERT_EQUAL_STRING(expected.c_str(), r.report().c_str());
}

static void test_counters_from_client_stats() {
  HealthRig r;
  r.health.setKeys(kHealthQueueDepth | kHealthDropped);
  // Overfill the ring while offline so the oldest data records are dropped.
  TEST_ASSERT_EQUAL(Result::Ok, r.c.begin(r.cfg));
  for (int i = 0; r.c.stats().dropped < 3; ++i) r.c.telemetry().add("n", i).send();
  Stats s = r.c.stats();
  r.c.loop();
  r.t.connack(0);
  r.t.subackAll();  // Online: the report is queued before the backlog drains

  std::string expected = "{\"vl_queue_depth\":" + std::to_string(s.queuedRecords) +
                         ",\"vl_dropped\":" + std::to_string(s.dropped) + "}";
  bool found = false;
  for (int i = 0; i < 1000 && r.c.stats().queuedRecords > 0; ++i) {
    std::vector<Published> out = r.flush();
    for (size_t j = 0; j < out.size(); ++j) {
      if (out[j].topic == "v1/devices/me/attributes") {
        TEST_ASSERT_EQUAL_STRING(expected.c_str(), out[j].payload.c_str());
        found = true;
      }
    }
  }
  TEST_ASSERT_TRUE(found);
}

static void test_nothing_during_provisioning() {
  HealthRig r;
  Provisioning prov;
  r.c.use(prov);
  TEST_ASSERT_EQUAL(Result::Ok, prov.setDevice("Meter-7", "KEY", "SECRET"));
  r.cfg.auth = Credentials();
  TEST_ASSERT_EQUAL(Result::Ok, prov.start(r.c, r.cfg));
  r.c.loop();
  r.t.connack(0);
  TEST_ASSERT_EQUAL(State::Online, r.c.state());
  r.c.loop();
  r.clock.advance(60000);
  r.c.loop();
  for (size_t i = 0; i < r.t.published.size(); ++i) {
    TEST_ASSERT_EQUAL_STRING("/provision/request", r.t.published[i].topic.c_str());
  }
  TEST_ASSERT_EQUAL_UINT(0, r.c.stats().queuedRecords);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_full_report_on_online);
  RUN_TEST(test_interval_reports_only_changing_keys);
  RUN_TEST(test_interval_zero_reports_only_on_connect);
  RUN_TEST(test_nothing_while_offline);
  RUN_TEST(test_reconnect_sends_full_report_and_restarts_timer);
  RUN_TEST(test_keys_switched_off);
  RUN_TEST(test_v1_key_names);
  RUN_TEST(test_unknown_platform_values_omitted);
  RUN_TEST(test_counters_from_client_stats);
  RUN_TEST(test_nothing_during_provisioning);
  return UNITY_END();
}
