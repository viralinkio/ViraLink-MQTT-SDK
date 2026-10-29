// 1.0.2 shim core: 1.0.2 topics and payloads onto v2 records, incoming
// messages to the 1.0.2 callback, attribute requests, gateway sends.
#include <string.h>
#include <unity.h>

#include <string>
#include <vector>

#include "ClientRig.h"
#include "vl/api/Gateway.h"
#include "vl/api/Rpc.h"
#include "vl/compat/Bridge.h"

using namespace vl;
using vltest::ClientRig;
using vltest::Published;

void setUp() {}
void tearDown() {}

namespace {

const uint64_t kUtcZero = 1767225600000ull;  // FakeClock starts at uptime 1000

struct Seen {
  std::string topic, payload;
};
std::vector<Seen> seen;
bool handled = true;

bool onMessage(const char* topic, const uint8_t* p, size_t n, void* ctx) {
  Seen s;
  s.topic = topic;
  s.payload.assign(reinterpret_cast<const char*>(p), n);
  static_cast<std::vector<Seen>*>(ctx)->push_back(s);
  return handled;
}

struct BridgeRig : ClientRig {
  compat::Bridge bridge;
  std::vector<Seen> responses;
  // attach false: the caller attaches the bridge (with a gateway).
  explicit BridgeRig(bool utc = true, bool attach = true) {
    seen.clear();
    handled = true;
    clock.haveUtc = utc;
    clock.utcAtZero = kUtcZero;
    bridge.onMessage(onMessage, &seen);
    if (attach) c.use(bridge);
  }
  Result publish(const char* topic, const std::string& payload) {
    return bridge.publish(topic, payload.data(), payload.size());
  }
  // flush() until the outbox is empty (at most maxInFlight go per round).
  std::vector<Published> drain() {
    std::vector<Published> all;
    for (int i = 0; i < 10; ++i) {
      std::vector<Published> p = flush();
      if (p.empty()) break;
      all.insert(all.end(), p.begin(), p.end());
    }
    return all;
  }
};

void expect(const Published& p, const char* topic, const char* payload) {
  TEST_ASSERT_EQUAL_STRING(topic, p.topic.c_str());
  TEST_ASSERT_EQUAL_STRING(payload, p.payload.c_str());
}

}  // namespace

static void test_device_topics_map_onto_records() {
  BridgeRig r;
  r.connect();
  TEST_ASSERT_EQUAL(Result::Queued, r.publish("v1/devices/me/telemetry", "{\"t\":1}"));
  TEST_ASSERT_EQUAL(Result::Queued, r.publish("v1/devices/me/attributes", "{\"fw\":\"1.0\"}"));
  TEST_ASSERT_EQUAL(Result::Queued, r.publish("v1/devices/me/attributes/request/7", "{\"sharedKeys\":\"a\"}"));
  TEST_ASSERT_EQUAL(Result::Queued, r.publish("v1/devices/me/rpc/request/3", "{\"method\":\"m\",\"params\":{}}"));
  TEST_ASSERT_EQUAL(Result::Queued, r.publish("v1/devices/me/claim", "{\"secretKey\":\"k\",\"durationMs\":60000}"));
  std::vector<Published> p = r.drain();
  TEST_ASSERT_EQUAL_UINT(5, p.size());
  expect(p[0], "v1/devices/me/telemetry", "{\"ts\":1767225601000,\"values\":{\"t\":1}}");
  expect(p[1], "v1/devices/me/attributes", "{\"fw\":\"1.0\"}");
  expect(p[2], "v1/devices/me/attributes/request/7", "{\"sharedKeys\":\"a\"}");
  expect(p[3], "v1/devices/me/rpc/request/3", "{\"method\":\"m\",\"params\":{}}");
  expect(p[4], "v1/devices/me/claim", "{\"secretKey\":\"k\",\"durationMs\":60000}");
}

static void test_rpc_reply_jumps_the_queue() {
  BridgeRig r;
  TEST_ASSERT_EQUAL(Result::Queued, r.publish("v1/devices/me/telemetry", "{\"t\":1}"));
  TEST_ASSERT_EQUAL(Result::Queued, r.publish("v1/devices/me/rpc/response/42", "{\"ok\":true}"));
  r.connect();
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(2, p.size());
  expect(p[0], "v1/devices/me/rpc/response/42", "{\"ok\":true}");
  TEST_ASSERT_EQUAL_STRING("v1/devices/me/telemetry", p[1].topic.c_str());
}

static void test_telemetry_keeps_its_own_timestamps() {
  BridgeRig r;
  r.connect();
  TEST_ASSERT_EQUAL(Result::Queued, r.publish("v1/devices/me/telemetry", "{\"ts\":1767225500000,\"values\":{\"t\":1}}"));
  TEST_ASSERT_EQUAL(Result::Queued,
                    r.publish("v1/devices/me/telemetry", "[{\"ts\":1767225500001,\"values\":{\"t\":2}},{\"t\":3}]"));
  // An object that only happens to have "ts" is plain values.
  TEST_ASSERT_EQUAL(Result::Queued, r.publish("v1/devices/me/telemetry", "{\"ts\":5}"));
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(4, p.size());
  expect(p[0], "v1/devices/me/telemetry", "{\"ts\":1767225500000,\"values\":{\"t\":1}}");
  expect(p[1], "v1/devices/me/telemetry", "{\"ts\":1767225500001,\"values\":{\"t\":2}}");
  expect(p[2], "v1/devices/me/telemetry", "{\"ts\":1767225601000,\"values\":{\"t\":3}}");
  expect(p[3], "v1/devices/me/telemetry", "{\"ts\":1767225601000,\"values\":{\"ts\":5}}");
}

static void test_refuses_what_v2_cannot_send() {
  BridgeRig r;
  r.connect();
  TEST_ASSERT_EQUAL(Result::Rejected, r.publish("v1/devices/me/telemetry", "not json"));
  TEST_ASSERT_EQUAL(Result::Rejected, r.publish("v1/devices/me/telemetry", "[]"));
  TEST_ASSERT_EQUAL(Result::Rejected, r.publish("v1/devices/me/telemetry", "[1]"));
  TEST_ASSERT_EQUAL(Result::Rejected, r.publish("v1/devices/me/telemetry", "[[{\"t\":1}]]"));
  TEST_ASSERT_EQUAL(Result::Rejected, r.publish("v1/devices/me/attributes", "[{\"a\":1}]"));
  TEST_ASSERT_EQUAL(Result::Rejected, r.publish("v1/devices/me/claim", "\"k\""));
  TEST_ASSERT_EQUAL(Result::Rejected, r.publish("v1/devices/me/rpc/response/", "{}"));
  TEST_ASSERT_EQUAL(Result::Rejected, r.publish("v1/devices/me/rpc/response/4x", "{}"));
  TEST_ASSERT_EQUAL(Result::Rejected, r.publish("v1/devices/me/rpc/response/99999999999", "{}"));
  TEST_ASSERT_EQUAL(Result::Rejected, r.publish("v1/devices/me/unknown", "{}"));
  TEST_ASSERT_EQUAL(Result::Rejected, r.publish("custom/topic", "{}"));
  // Gateway data needs a device name to be stamped and wrapped.
  TEST_ASSERT_EQUAL(Result::Rejected, r.publish("v1/gateway/telemetry", "{\"Dev\":[{\"t\":1}]}"));
  TEST_ASSERT_EQUAL(Result::Rejected, r.publish("v1/gateway/attributes", "{\"Dev\":{\"a\":1}}"));
  TEST_ASSERT_EQUAL(Result::Rejected, r.publish("v1/gateway/claim", "{\"Dev\":{}}"));
  // Sub-devices need a gateway.
  TEST_ASSERT_EQUAL(Result::NotReady, r.bridge.telemetry("Dev", "{\"t\":1}", 7));
  TEST_ASSERT_EQUAL(Result::NotReady, r.bridge.connectDevice("Dev"));
  TEST_ASSERT_EQUAL_UINT(0, r.flush().size());

  compat::Bridge loose;
  TEST_ASSERT_EQUAL(Result::NotReady, loose.publish("v1/devices/me/telemetry", "{}", 2));
}

static void test_raw_gateway_topics_pass_through() {
  BridgeRig r;
  r.connect();
  TEST_ASSERT_EQUAL(Result::Queued, r.publish("v1/gateway/connect", "{\"device\":\"Dev\"}"));
  TEST_ASSERT_EQUAL(Result::Queued, r.publish("v1/gateway/attributes/request",
                                              "{\"id\":1,\"device\":\"Dev\",\"client\":false,\"key\":\"a\"}"));
  TEST_ASSERT_EQUAL(Result::Queued, r.publish("v1/gateway/rpc", "{\"device\":\"Dev\",\"id\":5,\"data\":{}}"));
  TEST_ASSERT_EQUAL(Result::Queued, r.publish("v1/gateway/disconnect", "{\"device\":\"Dev\"}"));
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(4, p.size());
  expect(p[0], "v1/gateway/rpc", "{\"device\":\"Dev\",\"id\":5,\"data\":{}}");
  expect(p[1], "v1/gateway/connect", "{\"device\":\"Dev\"}");
  TEST_ASSERT_EQUAL_STRING("v1/gateway/attributes/request", p[2].topic.c_str());
  expect(p[3], "v1/gateway/disconnect", "{\"device\":\"Dev\"}");
}

static void test_incoming_messages_reach_the_callback() {
  BridgeRig r;
  r.connect();
  r.t.message("v1/devices/me/rpc/request/9", "{\"method\":\"restart_device\",\"params\":{\"seconds\":2}}");
  r.t.message("v1/devices/me/attributes", "{\"mode\":\"eco\"}");
  r.t.message("v1/devices/me/attributes/response/77", "{\"shared\":{}}");  // nobody waits for it
  TEST_ASSERT_EQUAL_UINT(3, seen.size());
  TEST_ASSERT_EQUAL_STRING("v1/devices/me/rpc/request/9", seen[0].topic.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"method\":\"restart_device\",\"params\":{\"seconds\":2}}", seen[0].payload.c_str());
  TEST_ASSERT_EQUAL_STRING("v1/devices/me/attributes", seen[1].topic.c_str());
  TEST_ASSERT_EQUAL_STRING("v1/devices/me/attributes/response/77", seen[2].topic.c_str());
  // No automatic RPC reply: 1.0.2 sketches answer with addToPublishQueue().
  TEST_ASSERT_EQUAL_UINT(0, r.flush().size());
}

static void test_unhandled_rpc_falls_through_to_other_modules() {
  BridgeRig r;
  Rpc<1> rpc;
  r.c.use(rpc);
  r.connect();
  handled = false;
  r.t.message("v1/devices/me/rpc/request/9", "{\"method\":\"nope\",\"params\":{}}");
  TEST_ASSERT_EQUAL_UINT(1, seen.size());
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(1, p.size());
  expect(p[0], "v1/devices/me/rpc/response/9", "{\"error\":\"unknown method: nope\"}");
}

static void test_attribute_request_gets_its_response() {
  BridgeRig r;
  r.connect();
  std::vector<Seen> got;
  TEST_ASSERT_EQUAL(Result::Queued, r.bridge.requestAttributes("{\"sharedKeys\":\"a\"}", 18, onMessage, &got));
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(1, p.size());
  const char* prefix = "v1/devices/me/attributes/request/";
  TEST_ASSERT_EQUAL(0, p[0].topic.compare(0, strlen(prefix), prefix));
  TEST_ASSERT_EQUAL_STRING("{\"sharedKeys\":\"a\"}", p[0].payload.c_str());
  std::string response = "v1/devices/me/attributes/response/" + p[0].topic.substr(strlen(prefix));
  r.t.message(response.c_str(), "{\"shared\":{\"a\":1}}");
  TEST_ASSERT_EQUAL_UINT(1, got.size());
  TEST_ASSERT_EQUAL_STRING(response.c_str(), got[0].topic.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"shared\":{\"a\":1}}", got[0].payload.c_str());
  TEST_ASSERT_EQUAL_UINT(0, seen.size());  // not also a stray message
  // A second response is a stray.
  r.t.message(response.c_str(), "{\"shared\":{\"a\":1}}");
  TEST_ASSERT_EQUAL_UINT(1, got.size());
  TEST_ASSERT_EQUAL_UINT(1, seen.size());
}

static void test_attribute_requests_time_out_silently_and_free_their_slot() {
  BridgeRig r;
  r.connect();
  std::vector<Seen> got;
  for (int i = 0; i < VL_COMPAT_MAX_REQUESTS; ++i) {
    TEST_ASSERT_EQUAL(Result::Queued, r.bridge.requestAttributes("{\"sharedKeys\":\"a\"}", 18, onMessage, &got));
  }
  TEST_ASSERT_EQUAL(Result::NoMemory, r.bridge.requestAttributes("{\"sharedKeys\":\"a\"}", 18, onMessage, &got));
  TEST_ASSERT_EQUAL(Result::Rejected, r.bridge.requestAttributes("a,b", 3, onMessage, &got));
  r.flush();
  r.clock.advance(VL_ATTR_TIMEOUT_MS);
  r.c.loop();
  TEST_ASSERT_EQUAL_UINT(0, got.size());
  TEST_ASSERT_EQUAL(Result::Queued, r.bridge.requestAttributes("{\"sharedKeys\":\"a\"}", 18, onMessage, &got));
  // end() drops outstanding requests silently too.
  r.c.end();
  TEST_ASSERT_EQUAL_UINT(0, got.size());
}

static void test_refused_request_frees_its_slot() {
  BridgeRig r;
  std::string big(r.c.maxPayload() + 1, ' ');
  big[0] = '{';
  big[big.size() - 1] = '}';
  r.connect();
  std::vector<Seen> got;
  for (int i = 0; i < VL_COMPAT_MAX_REQUESTS + 1; ++i) {
    TEST_ASSERT_EQUAL(Result::TooLarge, r.bridge.requestAttributes(big.data(), big.size(), onMessage, &got));
  }
  TEST_ASSERT_EQUAL(Result::Queued, r.bridge.requestAttributes("{\"sharedKeys\":\"a\"}", 18, onMessage, &got));
}

#if VL_ENABLE_GATEWAY
struct GwBridgeRig : BridgeRig {
  Gateway<2> gw;
  GwBridgeRig() : BridgeRig(false, false) {
    bridge.setGateway(gw);
    c.use(bridge, gw);
  }
};

static void test_gateway_sends_through_the_sub_device_table() {
  GwBridgeRig r;
  r.connect();
  TEST_ASSERT_EQUAL(Result::Queued, r.bridge.telemetry("Meter-1", "{\"kwh\":3.5}", 11));
  TEST_ASSERT_EQUAL(Result::Queued, r.bridge.attributes("Meter-1", "{\"fw\":\"1\"}", 10));
  TEST_ASSERT_EQUAL(Result::Queued, r.bridge.claim("Meter-1", "k", 60000));
  TEST_ASSERT_EQUAL(Result::Queued, r.bridge.disconnectDevice("Meter-1"));
  // Refused before the disconnected sub-device is connected again.
  TEST_ASSERT_EQUAL(Result::Rejected, r.bridge.telemetry("Meter-1", "[{\"kwh\":1}", 10));
  TEST_ASSERT_EQUAL(Result::Rejected, r.bridge.attributes("Meter-1", "[]", 2));
  std::vector<Published> p = r.drain();
  TEST_ASSERT_EQUAL_UINT(5, p.size());
  expect(p[0], "v1/gateway/connect", "{\"device\":\"Meter-1\"}");
  // No UTC yet: the drain leaves out the stamp it cannot convert.
  expect(p[1], "v1/gateway/telemetry", "{\"Meter-1\":[{\"kwh\":3.5}]}");
  expect(p[2], "v1/gateway/attributes", "{\"Meter-1\":{\"fw\":\"1\"}}");
  expect(p[3], "v1/gateway/claim", "{\"Meter-1\":{\"secretKey\":\"k\",\"durationMs\":60000}}");
  expect(p[4], "v1/gateway/disconnect", "{\"device\":\"Meter-1\"}");

  TEST_ASSERT_EQUAL(Result::Ok, r.bridge.connectDevice("Meter-2"));
  TEST_ASSERT_EQUAL(Result::NoMemory, r.bridge.connectDevice("Meter-3"));
  TEST_ASSERT_EQUAL(Result::NoMemory, r.bridge.telemetry("Meter-3", "{\"kwh\":1}", 9));
}

static void test_gateway_messages_reach_the_callback() {
  GwBridgeRig r;
  r.connect();
  r.t.message("v1/gateway/rpc", "{\"device\":\"Meter-1\",\"data\":{\"id\":1,\"method\":\"m\",\"params\":{}}}");
  r.t.message("v1/gateway/attributes", "{\"device\":\"Meter-1\",\"data\":{\"a\":1}}");
  TEST_ASSERT_EQUAL_UINT(2, seen.size());
  TEST_ASSERT_EQUAL_STRING("v1/gateway/rpc", seen[0].topic.c_str());
  TEST_ASSERT_EQUAL_STRING("v1/gateway/attributes", seen[1].topic.c_str());
  // The bridge took the RPC, so the gateway sent no automatic error reply.
  TEST_ASSERT_EQUAL_UINT(0, r.flush().size());
}

static void test_gateway_topics_only_with_a_gateway() {
  BridgeRig plain;
  plain.c.begin(plain.cfg);
  plain.c.loop();
  plain.t.connack(0);
  for (size_t i = 0; i < plain.t.subscribed.size(); ++i) {
    TEST_ASSERT_TRUE(plain.t.subscribed[i].compare(0, 11, "v1/gateway/") != 0);
  }
  GwBridgeRig gw;
  gw.c.begin(gw.cfg);
  gw.c.loop();
  gw.t.connack(0);
  bool rpc = false;
  for (size_t i = 0; i < gw.t.subscribed.size(); ++i) rpc |= gw.t.subscribed[i] == "v1/gateway/rpc";
  TEST_ASSERT_TRUE(rpc);
}
#endif

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_device_topics_map_onto_records);
  RUN_TEST(test_rpc_reply_jumps_the_queue);
  RUN_TEST(test_telemetry_keeps_its_own_timestamps);
  RUN_TEST(test_refuses_what_v2_cannot_send);
  RUN_TEST(test_raw_gateway_topics_pass_through);
  RUN_TEST(test_incoming_messages_reach_the_callback);
  RUN_TEST(test_unhandled_rpc_falls_through_to_other_modules);
  RUN_TEST(test_attribute_request_gets_its_response);
  RUN_TEST(test_attribute_requests_time_out_silently_and_free_their_slot);
  RUN_TEST(test_refused_request_frees_its_slot);
#if VL_ENABLE_GATEWAY
  RUN_TEST(test_gateway_sends_through_the_sub_device_table);
  RUN_TEST(test_gateway_messages_reach_the_callback);
  RUN_TEST(test_gateway_topics_only_with_a_gateway);
#endif
  return UNITY_END();
}
