// Gateway: sub-device table, connect replay, data, claim, RPC and attributes.
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include <string>
#include <vector>

#include "ClientRig.h"
#include "vl/api/Gateway.h"

using namespace vl;
using vltest::ClientRig;
using vltest::Published;

void setUp() {}
void tearDown() {}

namespace {

struct GwRig : ClientRig {
  Gateway<2> gw;
  GwRig() { c.use(gw); }
  // Brings the session back Online after t.close().
  void reconnect() {
    clock.advance(2000);
    c.loop();
    t.connack(0);
    t.subackAll();
    TEST_ASSERT_EQUAL(State::Online, c.state());
  }
  // Delivers a gateway RPC and returns the single reply it produced.
  std::string rpc(const char* body) {
    t.message("v1/gateway/rpc", body);
    std::vector<Published> p = flush();
    TEST_ASSERT_EQUAL_UINT(1, p.size());
    TEST_ASSERT_EQUAL_STRING("v1/gateway/rpc", p[0].topic.c_str());
    return p[0].payload;
  }
};

void expect(const Published& p, const char* topic, const char* payload) {
  TEST_ASSERT_EQUAL_STRING(topic, p.topic.c_str());
  TEST_ASSERT_EQUAL_STRING(payload, p.payload.c_str());
}

void onSetValve(const char* device, const RpcRequest& req, RpcReply& reply, void*) {
  reply.json().beginObject().key("device").value(device).key("open").value(req.params()["open"].asBool()).endObject();
  reply.send();
}
void silent(const char*, const RpcRequest&, RpcReply&, void*) {}

std::vector<std::string> updates;
void onGwUpdate(const char* device, const char* key, JsonValue v, void*) {
  updates.push_back(std::string(device) + "/" + key + "=" +
                    (v.valid() ? std::string(v.raw(), v.rawSize()) : std::string("<deleted>")));
}

struct GwResponse {
  int n = 0;
  Result r = Result::Ok;
  std::string device, fw, value;
} gwResp;
void onGwResponse(Result r, JsonValue response, void*) {
  ++gwResp.n;
  gwResp.r = r;
  char buf[16] = "";
  response["device"].copyString(buf, sizeof(buf));
  gwResp.device = buf;
  buf[0] = '\0';
  response["values"]["fw"].copyString(buf, sizeof(buf));
  gwResp.fw = buf;
  buf[0] = '\0';
  response["value"].copyString(buf, sizeof(buf));
  gwResp.value = buf;
}

uint32_t requestId(const Published& p) {
  TEST_ASSERT_EQUAL_STRING("v1/gateway/attributes/request", p.topic.c_str());
  return static_cast<uint32_t>(JsonValue::parse(p.payload.data(), p.payload.size())["id"].asInt());
}

}  // namespace

static void test_subscribes_gateway_topics() {
  GwRig r;
  r.c.begin(r.cfg);
  r.c.loop();
  r.t.connack(0);
  bool rpc = false, upd = false, res = false;
  for (size_t i = 0; i < r.t.subscribed.size(); ++i) {
    rpc |= r.t.subscribed[i] == "v1/gateway/rpc";
    upd |= r.t.subscribed[i] == "v1/gateway/attributes";
    res |= r.t.subscribed[i] == "v1/gateway/attributes/response";
  }
  TEST_ASSERT_TRUE(rpc && upd && res);
}

static void test_connect_precedes_data() {
  GwRig r;
  r.connect();
  SubDevice m = r.gw.device("Meter-12", "energy-meter");
  TEST_ASSERT_TRUE(m.valid());
  TEST_ASSERT_EQUAL_STRING("Meter-12", m.name());
  TEST_ASSERT_EQUAL(Result::Queued, m.telemetry().add("kwh", 3.5).send());
  TEST_ASSERT_EQUAL(Result::Queued, m.telemetry().add("kwh", 3.75).ts(1700000000000ULL).send());
  TEST_ASSERT_EQUAL(Result::Queued, m.attributes().add("fw", "1.0").send());
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(4, p.size());
  expect(p[0], "v1/gateway/connect", "{\"device\":\"Meter-12\",\"type\":\"energy-meter\"}");
  expect(p[1], "v1/gateway/telemetry", "{\"Meter-12\":[{\"kwh\":3.5}]}");
  expect(p[2], "v1/gateway/telemetry", "{\"Meter-12\":[{\"ts\":1700000000000,\"values\":{\"kwh\":3.75}}]}");
  expect(p[3], "v1/gateway/attributes", "{\"Meter-12\":{\"fw\":\"1.0\"}}");
  TEST_ASSERT_EQUAL(Result::Queued, m.claim("secret", 60000));
  p = r.flush();
  TEST_ASSERT_EQUAL_UINT(1, p.size());
  expect(p[0], "v1/gateway/claim", "{\"Meter-12\":{\"secretKey\":\"secret\",\"durationMs\":60000}}");
}

static void test_connect_once_and_disconnect() {
  GwRig r;
  r.connect();
  r.gw.device("A");
  r.gw.device("A", "ignored");  // already connected: nothing queued
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(1, p.size());
  expect(p[0], "v1/gateway/connect", "{\"device\":\"A\"}");
  TEST_ASSERT_EQUAL(Result::Queued, r.gw.disconnect("A"));
  TEST_ASSERT_EQUAL(Result::Ok, r.gw.disconnect("A"));  // already disconnected: nothing queued
  TEST_ASSERT_EQUAL(Result::Rejected, r.gw.disconnect("nobody"));
  p = r.flush();
  TEST_ASSERT_EQUAL_UINT(1, p.size());
  expect(p[0], "v1/gateway/disconnect", "{\"device\":\"A\"}");
  TEST_ASSERT_TRUE(r.gw.find("A").valid());  // the slot stays
  r.gw.device("A");
  p = r.flush();
  TEST_ASSERT_EQUAL_UINT(1, p.size());
  expect(p[0], "v1/gateway/connect", "{\"device\":\"A\"}");
}

static void test_queued_records_keep_their_device() {
  GwRig r;
  r.connect();
  SubDevice a = r.gw.device("A");
  a.telemetry().add("n", 1).send();
  TEST_ASSERT_EQUAL(Result::Queued, a.disconnect());
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(3, p.size());
  expect(p[0], "v1/gateway/connect", "{\"device\":\"A\"}");
  expect(p[1], "v1/gateway/telemetry", "{\"A\":[{\"n\":1}]}");
  expect(p[2], "v1/gateway/disconnect", "{\"device\":\"A\"}");
}

static void test_replays_connected_devices_on_reconnect() {
  GwRig r;
  r.connect();
  SubDevice a = r.gw.device("A", "meter");
  r.gw.device("B");
  r.gw.disconnect("B");
  TEST_ASSERT_EQUAL_UINT(3, r.flush().size());
  r.t.close();
  a.telemetry().add("n", 1).send();  // queued while offline
  r.reconnect();
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(2, p.size());
  expect(p[0], "v1/gateway/connect", "{\"device\":\"A\",\"type\":\"meter\"}");
  expect(p[1], "v1/gateway/telemetry", "{\"A\":[{\"n\":1}]}");
}

static void test_table_capacity_and_validation() {
  GwRig r;
  TEST_ASSERT_TRUE(r.gw.device("a").valid());
  TEST_ASSERT_TRUE(r.gw.device("b").valid());
  SubDevice full = r.gw.device("c");
  TEST_ASSERT_EQUAL(Result::NoMemory, full.status());
  TEST_ASSERT_EQUAL(Result::NoMemory, full.telemetry().add("x", 1).send());
  TEST_ASSERT_EQUAL(Result::NoMemory, full.claim(nullptr, 1000));
  TEST_ASSERT_EQUAL_STRING("", full.name());
  TEST_ASSERT_EQUAL(Result::Rejected, r.gw.device(nullptr).status());
  TEST_ASSERT_EQUAL(Result::Rejected, r.gw.device("").status());
  std::string longName(VL_GW_NAME_SIZE, 'n');
  TEST_ASSERT_EQUAL(Result::Rejected, r.gw.device(longName.c_str()).status());
  std::string longType(VL_GW_TYPE_SIZE, 't');
  TEST_ASSERT_EQUAL(Result::Rejected, r.gw.device("a", longType.c_str()).status());
  TEST_ASSERT_EQUAL(Result::Rejected, r.gw.find("zz").status());
  Gateway<1> loose;  // never attached
  TEST_ASSERT_EQUAL(Result::NotReady, loose.device("a").status());
}

// Also guards the destructor order: without ~GatewayBase detaching, ASan
// reports the drain reading the destroyed gateway's table.
static void test_gateway_destroyed_with_queued_records() {
  ClientRig r;
  {
    Gateway<1> gw;
    r.c.use(gw);
    r.connect();
    gw.device("A").telemetry().add("n", 1).send();
  }
  // The connect event carries its own name; the telemetry can no longer be addressed.
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(1, p.size());
  expect(p[0], "v1/gateway/connect", "{\"device\":\"A\"}");
  TEST_ASSERT_EQUAL_UINT32(1, r.c.stats().deadLetters);
}

static void test_rpc_dispatch_and_reply() {
  GwRig r;
  TEST_ASSERT_EQUAL(Result::Ok, r.gw.on("setValve", onSetValve));
  r.connect();
  TEST_ASSERT_EQUAL_STRING(
      "{\"device\":\"Valve-3\",\"id\":7,\"data\":{\"device\":\"Valve-3\",\"open\":true}}",
      r.rpc("{\"device\":\"Valve-3\",\"data\":{\"id\":7,\"method\":\"setValve\",\"params\":{\"open\":true}}}").c_str());
}

static void test_rpc_automatic_replies() {
  GwRig r;
  r.gw.on("silent", silent);
  r.connect();
  TEST_ASSERT_EQUAL_STRING("{\"device\":\"V\",\"id\":1,\"data\":{\"error\":\"unknown method: nope\"}}",
                           r.rpc("{\"device\":\"V\",\"data\":{\"id\":1,\"method\":\"nope\"}}").c_str());
  TEST_ASSERT_EQUAL_STRING("{\"device\":\"V\",\"id\":2,\"data\":{\"error\":\"no reply\"}}",
                           r.rpc("{\"device\":\"V\",\"data\":{\"id\":2,\"method\":\"silent\"}}").c_str());
  TEST_ASSERT_EQUAL_STRING("{\"device\":\"V\",\"id\":3,\"data\":{\"error\":\"bad request\"}}",
                           r.rpc("{\"device\":\"V\",\"data\":{\"id\":3}}").c_str());
  // Without a device or an id there is nobody to answer.
  r.t.message("v1/gateway/rpc", "{\"data\":{\"id\":4,\"method\":\"silent\"}}");
  r.t.message("v1/gateway/rpc", "{\"device\":\"V\",\"data\":{\"method\":\"silent\"}}");
  r.t.message("v1/gateway/rpc", "not json");
  TEST_ASSERT_EQUAL_UINT(0, r.flush().size());
}

static void test_rpc_reply_goes_first() {
  GwRig r;
  r.gw.on("setValve", onSetValve);
  r.connect();
  r.gw.device("A").telemetry().add("n", 1).send();
  r.t.message("v1/gateway/rpc", "{\"device\":\"A\",\"data\":{\"id\":1,\"method\":\"setValve\",\"params\":{\"open\":false}}}");
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(3, p.size());
  expect(p[0], "v1/gateway/rpc", "{\"device\":\"A\",\"id\":1,\"data\":{\"device\":\"A\",\"open\":false}}");
  TEST_ASSERT_EQUAL_STRING("v1/gateway/connect", p[1].topic.c_str());
}

static void test_rpc_registry_validation() {
  Gateway<1, 1> g;
  TEST_ASSERT_EQUAL(Result::Rejected, g.on(nullptr, silent));
  TEST_ASSERT_EQUAL(Result::Rejected, g.on("a", nullptr));
  std::string longName(VL_RPC_METHOD_SIZE, 'm');
  TEST_ASSERT_EQUAL(Result::Rejected, g.on(longName.c_str(), silent));
  TEST_ASSERT_EQUAL(Result::Ok, g.on("a", silent));
  TEST_ASSERT_EQUAL(Result::Ok, g.on("a", onSetValve));  // replaces
  TEST_ASSERT_EQUAL(Result::NoMemory, g.on("b", silent));
}

static void test_attribute_updates_per_device() {
  GwRig r;
  updates.clear();
  r.gw.onAttributeUpdate(onGwUpdate);
  r.connect();
  r.t.message("v1/gateway/attributes", "{\"device\":\"A\",\"data\":{\"mode\":\"eco\",\"deleted\":[\"old\"]}}");
  TEST_ASSERT_EQUAL_UINT(2, updates.size());
  TEST_ASSERT_EQUAL_STRING("A/mode=\"eco\"", updates[0].c_str());
  TEST_ASSERT_EQUAL_STRING("A/old=<deleted>", updates[1].c_str());
  r.t.message("v1/gateway/attributes", "{\"device\":\"A\"}");
  r.t.message("v1/gateway/attributes", "{\"data\":{\"k\":1}}");
  r.t.message("v1/gateway/attributes", "[]");
  TEST_ASSERT_EQUAL_UINT(2, updates.size());
}

static void test_attribute_request_and_response() {
  GwRig r;
  r.connect();
  SubDevice a = r.gw.device("A");
  r.flush();
  gwResp = GwResponse();
  TEST_ASSERT_EQUAL(Result::Queued, a.requestAttributes(true, "fw,model", onGwResponse, nullptr));
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(1, p.size());
  unsigned long id = requestId(p[0]);
  char text[128];
  snprintf(text, sizeof(text), "{\"id\":%lu,\"device\":\"A\",\"client\":true,\"keys\":[\"fw\",\"model\"]}", id);
  TEST_ASSERT_EQUAL_STRING(text, p[0].payload.c_str());
  snprintf(text, sizeof(text), "{\"id\":%lu,\"device\":\"A\",\"values\":{\"fw\":\"1.0\",\"model\":\"X\"}}", id);
  r.t.message("v1/gateway/attributes/response", text);
  TEST_ASSERT_EQUAL(1, gwResp.n);
  TEST_ASSERT_EQUAL(Result::Ok, gwResp.r);
  TEST_ASSERT_EQUAL_STRING("A", gwResp.device.c_str());
  TEST_ASSERT_EQUAL_STRING("1.0", gwResp.fw.c_str());

  // One attribute found: the server answers with "value".
  TEST_ASSERT_EQUAL(Result::Queued, a.requestAttributes(false, "mode", onGwResponse, nullptr));
  p = r.flush();
  id = requestId(p[0]);
  snprintf(text, sizeof(text), "{\"id\":%lu,\"device\":\"A\",\"value\":\"eco\"}", id);
  r.t.message("v1/gateway/attributes/response", text);
  TEST_ASSERT_EQUAL(2, gwResp.n);
  TEST_ASSERT_EQUAL_STRING("eco", gwResp.value.c_str());

  r.t.message("v1/gateway/attributes/response", "{\"id\":999,\"device\":\"A\",\"value\":1}");  // unknown id
  r.t.message("v1/gateway/attributes/response", "not json");
  TEST_ASSERT_EQUAL(2, gwResp.n);
}

static void test_attribute_request_timeout_and_limits() {
  GwRig r;
  r.connect();
  SubDevice a = r.gw.device("A");
  TEST_ASSERT_EQUAL(Result::Rejected, a.requestAttributes(true, nullptr, onGwResponse, nullptr));
  TEST_ASSERT_EQUAL(Result::Rejected, a.requestAttributes(true, "", onGwResponse, nullptr));
  TEST_ASSERT_EQUAL(Result::Rejected, a.requestAttributes(true, "fw", nullptr, nullptr));
  TEST_ASSERT_EQUAL(Result::Rejected, r.gw.find("zz").requestAttributes(true, "fw", onGwResponse, nullptr));
  std::string many = "key";
  for (int i = 0; i < 200; ++i) many += ",key";
  // Too large: the request is opened, then abandoned, so its slot comes back.
  TEST_ASSERT_EQUAL(Result::TooLarge, a.requestAttributes(true, many.c_str(), onGwResponse, nullptr));
  gwResp = GwResponse();
  for (int i = 0; i < VL_GW_MAX_REQUESTS; ++i) {
    TEST_ASSERT_EQUAL(Result::Queued, a.requestAttributes(true, "fw", onGwResponse, nullptr));
  }
  TEST_ASSERT_EQUAL(Result::NoMemory, a.requestAttributes(true, "fw", onGwResponse, nullptr));
  r.clock.advance(VL_ATTR_TIMEOUT_MS);
  r.c.loop();
  TEST_ASSERT_EQUAL(VL_GW_MAX_REQUESTS, gwResp.n);
  TEST_ASSERT_EQUAL(Result::Timeout, gwResp.r);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_subscribes_gateway_topics);
  RUN_TEST(test_connect_precedes_data);
  RUN_TEST(test_connect_once_and_disconnect);
  RUN_TEST(test_queued_records_keep_their_device);
  RUN_TEST(test_replays_connected_devices_on_reconnect);
  RUN_TEST(test_table_capacity_and_validation);
  RUN_TEST(test_gateway_destroyed_with_queued_records);
  RUN_TEST(test_rpc_dispatch_and_reply);
  RUN_TEST(test_rpc_automatic_replies);
  RUN_TEST(test_rpc_reply_goes_first);
  RUN_TEST(test_rpc_registry_validation);
  RUN_TEST(test_attribute_updates_per_device);
  RUN_TEST(test_attribute_request_and_response);
  RUN_TEST(test_attribute_request_timeout_and_limits);
  return UNITY_END();
}
