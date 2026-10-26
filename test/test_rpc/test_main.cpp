// Server-side and client-side RPC over the fake transport.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include <string>

#include "ClientRig.h"
#include "vl/api/Rpc.h"

using namespace vl;
using vltest::ClientRig;
using vltest::Published;

void setUp() {}
void tearDown() {}

namespace {

struct RpcRig : ClientRig {
  Rpc<4> rpc;
  RpcRig() { c.use(rpc); }
  // Delivers a server RPC and returns the single reply it produced.
  std::string reply(uint32_t id, const char* body) {
    char topic[64];
    snprintf(topic, sizeof(topic), "v1/devices/me/rpc/request/%lu", static_cast<unsigned long>(id));
    t.message(topic, body);
    std::vector<Published> p = flush();
    TEST_ASSERT_EQUAL_UINT(1, p.size());
    snprintf(topic, sizeof(topic), "v1/devices/me/rpc/response/%lu", static_cast<unsigned long>(id));
    TEST_ASSERT_EQUAL_STRING(topic, p[0].topic.c_str());
    return p[0].payload;
  }
};

void onSetRelay(const RpcRequest& req, RpcReply& reply, void* ctx) {
  bool* relay = static_cast<bool*>(ctx);
  *relay = req.params()["on"].asBool();
  reply.json().beginObject().key("on").value(*relay).endObject();
  reply.send();
}

void ignore(const RpcRequest&, RpcReply&, void*) {}

Result secondSend = Result::Ok;
void sendTwice(const RpcRequest&, RpcReply& reply, void*) {
  reply.send("{\"t\":1}");
  secondSend = reply.send("{\"t\":2}");
}

void busy(const RpcRequest&, RpcReply& reply, void*) { reply.error("busy"); }

void huge(const RpcRequest&, RpcReply& reply, void*) {
  std::string big(VL_RPC_REPLY_SIZE, 'x');
  reply.json().beginObject().key("k").value(big.c_str()).endObject();
  TEST_ASSERT_EQUAL(Result::TooLarge, reply.send());
}

}  // namespace

static void test_subscribes_rpc_topics() {
  RpcRig r;
  r.c.begin(r.cfg);
  r.c.loop();
  r.t.connack(0);
  bool req = false, resp = false;
  for (size_t i = 0; i < r.t.subscribed.size(); ++i) {
    req |= r.t.subscribed[i] == "v1/devices/me/rpc/request/+";
    resp |= r.t.subscribed[i] == "v1/devices/me/rpc/response/+";
  }
  TEST_ASSERT_TRUE(req && resp);
}

static void test_dispatches_and_replies() {
  RpcRig r;
  bool relay = false;
  TEST_ASSERT_EQUAL(Result::Ok, r.rpc.on("setRelay", onSetRelay, &relay));
  r.connect();
  TEST_ASSERT_EQUAL_STRING("{\"on\":true}",
                           r.reply(42, "{\"method\":\"setRelay\",\"params\":{\"on\":true}}").c_str());
  TEST_ASSERT_TRUE(relay);
}

static void test_error_replies() {
  RpcRig r;
  r.rpc.on("ignore", ignore);
  r.rpc.on("busy", busy);
  r.rpc.on("huge", huge);
  r.connect();
  TEST_ASSERT_EQUAL_STRING("{\"error\":\"unknown method: nope\"}", r.reply(7, "{\"method\":\"nope\"}").c_str());
  TEST_ASSERT_EQUAL_STRING("{\"error\":\"bad request\"}", r.reply(8, "not json").c_str());
  TEST_ASSERT_EQUAL_STRING("{\"error\":\"bad request\"}", r.reply(9, "{\"params\":1}").c_str());
  TEST_ASSERT_EQUAL_STRING("{\"error\":\"no reply\"}", r.reply(10, "{\"method\":\"ignore\"}").c_str());
  TEST_ASSERT_EQUAL_STRING("{\"error\":\"busy\"}", r.reply(11, "{\"method\":\"busy\"}").c_str());
  TEST_ASSERT_EQUAL_STRING("{\"error\":\"reply too large\"}", r.reply(12, "{\"method\":\"huge\"}").c_str());
}

static void test_send_once() {
  RpcRig r;
  r.rpc.on("twice", sendTwice);
  r.connect();
  TEST_ASSERT_EQUAL_STRING("{\"t\":1}", r.reply(1, "{\"method\":\"twice\"}").c_str());
  TEST_ASSERT_EQUAL(Result::Rejected, secondSend);
}

static void test_reply_jumps_queue() {
  RpcRig r;
  r.rpc.on("busy", busy);
  r.connect();
  for (int i = 0; i < 3; ++i) r.c.telemetry().add("i", i).send();
  r.t.message("v1/devices/me/rpc/request/5", "{\"method\":\"busy\"}");
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(4, p.size());
  TEST_ASSERT_EQUAL_STRING("v1/devices/me/rpc/response/5", p[0].topic.c_str());
}

static void test_registry() {
  Rpc<2> rpc;
  TEST_ASSERT_EQUAL(Result::Ok, rpc.on("a", ignore));
  TEST_ASSERT_EQUAL(Result::Ok, rpc.on("b", ignore));
  TEST_ASSERT_EQUAL(Result::NoMemory, rpc.on("c", ignore));
  TEST_ASSERT_EQUAL(Result::Ok, rpc.on("a", busy));  // replaces
  std::string longName(VL_RPC_METHOD_SIZE, 'm');
  TEST_ASSERT_EQUAL(Result::Rejected, rpc.on(longName.c_str(), ignore));
  TEST_ASSERT_EQUAL(Result::Rejected, rpc.on("a", nullptr));
  TEST_ASSERT_EQUAL(Result::Rejected, rpc.on(nullptr, ignore));
}

namespace {
struct CallLog {
  int n = 0;
  Result r = Result::Ok;
  long long time = -1;
} calls;
void onCall(Result res, JsonValue v, void*) {
  ++calls.n;
  calls.r = res;
  calls.time = v["time"].asInt(-1);
}
}  // namespace

static void test_call_publishes_and_delivers_response() {
  RpcRig r;
  calls = CallLog();
  r.connect();
  TEST_ASSERT_EQUAL(Result::Queued, r.rpc.call("getTime", "{\"tz\":0}", onCall, nullptr));
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(1, p.size());
  const char* prefix = "v1/devices/me/rpc/request/";
  TEST_ASSERT_EQUAL_MEMORY(prefix, p[0].topic.c_str(), strlen(prefix));
  TEST_ASSERT_EQUAL_STRING("{\"method\":\"getTime\",\"params\":{\"tz\":0}}", p[0].payload.c_str());
  std::string resp = "v1/devices/me/rpc/response/" + p[0].topic.substr(strlen(prefix));
  r.t.message(resp.c_str(), "{\"time\":123}");
  r.t.message(resp.c_str(), "{\"time\":456}");  // duplicate: ignored
  TEST_ASSERT_EQUAL(1, calls.n);
  TEST_ASSERT_EQUAL(Result::Ok, calls.r);
  TEST_ASSERT_EQUAL_INT(123, static_cast<int>(calls.time));
}

static void test_call_times_out_and_frees_slot() {
  RpcRig r;
  calls = CallLog();
  r.connect();
  for (int i = 0; i < VL_RPC_MAX_CALLS; ++i) {
    TEST_ASSERT_EQUAL(Result::Queued, r.rpc.call("x", nullptr, onCall, nullptr, 500));
  }
  TEST_ASSERT_EQUAL(Result::NoMemory, r.rpc.call("x", nullptr, onCall, nullptr, 500));
  r.clock.advance(500);
  r.c.loop();
  TEST_ASSERT_EQUAL(VL_RPC_MAX_CALLS, calls.n);
  TEST_ASSERT_EQUAL(Result::Timeout, calls.r);
  TEST_ASSERT_EQUAL_INT(-1, static_cast<int>(calls.time));
  TEST_ASSERT_EQUAL(Result::Queued, r.rpc.call("x", nullptr, onCall, nullptr, 500));
}

static void test_call_validates() {
  RpcRig r;
  TEST_ASSERT_EQUAL(Result::Rejected, r.rpc.call(nullptr, nullptr, onCall, nullptr));
  TEST_ASSERT_EQUAL(Result::Rejected, r.rpc.call("x", "not json", onCall, nullptr));
  Rpc<1> detached;
  TEST_ASSERT_EQUAL(Result::NotReady, detached.call("x", nullptr, onCall, nullptr));
}

// Natural declaration order (client, then module) destroys the module first.
static void test_module_destroyed_first_drops_its_calls() {
  ClientRig rig;
  calls = CallLog();
  {
    Rpc<1> rpc;
    rig.c.use(rpc);
    rig.connect();
    TEST_ASSERT_EQUAL(Result::Queued, rpc.call("x", nullptr, onCall, nullptr, 500));
  }
  rig.clock.advance(1000);
  rig.c.loop();
  rig.t.message("v1/devices/me/rpc/request/1", "{\"method\":\"x\"}");
  TEST_ASSERT_EQUAL(0, calls.n);
}

static void test_client_destroyed_first_leaves_module_safe() {
  Rpc<1> rpc;
  calls = CallLog();
  {
    ClientRig rig;
    rig.c.use(rpc);
    rig.connect();
    TEST_ASSERT_EQUAL(Result::Queued, rpc.call("x", nullptr, onCall, nullptr, 500));
  }
  TEST_ASSERT_EQUAL(1, calls.n);
  TEST_ASSERT_EQUAL(Result::NotReady, calls.r);
  TEST_ASSERT_EQUAL(Result::NotReady, rpc.call("x", nullptr, onCall, nullptr));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_subscribes_rpc_topics);
  RUN_TEST(test_dispatches_and_replies);
  RUN_TEST(test_error_replies);
  RUN_TEST(test_send_once);
  RUN_TEST(test_reply_jumps_queue);
  RUN_TEST(test_registry);
  RUN_TEST(test_call_publishes_and_delivers_response);
  RUN_TEST(test_call_times_out_and_frees_slot);
  RUN_TEST(test_call_validates);
  RUN_TEST(test_module_destroyed_first_drops_its_calls);
  RUN_TEST(test_client_destroyed_first_leaves_module_safe);
  return UNITY_END();
}
