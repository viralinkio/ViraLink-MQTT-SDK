// Attribute requests, shared-attribute updates and binding, client-attribute
// reporting.
#include <math.h>
#include <string.h>
#include <unity.h>

#include <string>
#include <vector>

#include "ClientRig.h"
#include "vl/api/Attributes.h"

using namespace vl;
using vltest::ClientRig;
using vltest::Published;

void setUp() {}
void tearDown() {}

namespace {

struct AttrRig : ClientRig {
  SharedAttributes<4> shared;
  ClientAttributes<4> attrs;
  AttrRig() { c.use(shared, attrs); }
  void update(const char* json) { t.message("v1/devices/me/attributes", json); }
};

std::vector<std::string> seenA, seenB;
void record(std::vector<std::string>& out, const char* key, JsonValue v) {
  out.push_back(std::string(key) + "=" + (v.valid() ? std::string(v.raw(), v.rawSize()) : std::string("<deleted>")));
}
void onA(const char* key, JsonValue v, void*) { record(seenA, key, v); }
void onB(const char* key, JsonValue v, void*) { record(seenB, key, v); }

struct ResponseLog {
  int n = 0;
  Result r = Result::Ok;
  std::string fw, mode;
} resp;
void onResponse(Result r, JsonValue client, JsonValue shared, void*) {
  ++resp.n;
  resp.r = r;
  char buf[16] = "";
  client["fw"].copyString(buf, sizeof(buf));
  resp.fw = buf;
  buf[0] = '\0';
  shared["mode"].copyString(buf, sizeof(buf));
  resp.mode = buf;
}

std::string requestId(const Published& p) {
  const char* prefix = "v1/devices/me/attributes/request/";
  TEST_ASSERT_EQUAL_MEMORY(prefix, p.topic.c_str(), strlen(prefix));
  return p.topic.substr(strlen(prefix));
}

}  // namespace

static void test_subscribes_attribute_topics() {
  AttrRig r;
  r.c.begin(r.cfg);
  r.c.loop();
  r.t.connack(0);
  bool upd = false, res = false;
  for (size_t i = 0; i < r.t.subscribed.size(); ++i) {
    upd |= r.t.subscribed[i] == "v1/devices/me/attributes";
    res |= r.t.subscribed[i] == "v1/devices/me/attributes/response/+";
  }
  TEST_ASSERT_TRUE(upd && res);
}

static void test_update_handlers_per_key_and_deletion() {
  AttrRig r;
  seenA.clear();
  seenB.clear();
  TEST_ASSERT_EQUAL(Result::Ok, r.shared.onUpdate("mode,interval", onA));
  TEST_ASSERT_EQUAL(Result::Ok, r.shared.onUpdate(nullptr, onB));
  r.connect();
  r.update("{\"mode\":\"eco\",\"other\":1}");
  TEST_ASSERT_EQUAL_UINT(1, seenA.size());
  TEST_ASSERT_EQUAL_STRING("mode=\"eco\"", seenA[0].c_str());
  TEST_ASSERT_EQUAL_UINT(2, seenB.size());
  TEST_ASSERT_EQUAL_STRING("other=1", seenB[1].c_str());
  r.update("{\"deleted\":[\"interval\",\"x\"]}");
  TEST_ASSERT_EQUAL_UINT(2, seenA.size());
  TEST_ASSERT_EQUAL_STRING("interval=<deleted>", seenA[1].c_str());
  TEST_ASSERT_EQUAL_UINT(4, seenB.size());
  r.update("garbage");  // ignored
  TEST_ASSERT_EQUAL_UINT(4, seenB.size());
}

static void test_request_and_response() {
  AttrRig r;
  resp = ResponseLog();
  r.connect();
  TEST_ASSERT_EQUAL(Result::Queued, r.shared.request("fw,door", "mode", onResponse, nullptr));
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(1, p.size());
  TEST_ASSERT_EQUAL_STRING("{\"clientKeys\":\"fw,door\",\"sharedKeys\":\"mode\"}", p[0].payload.c_str());
  std::string topic = "v1/devices/me/attributes/response/" + requestId(p[0]);
  r.t.message(topic.c_str(), "{\"client\":{\"fw\":\"1.0\"},\"shared\":{\"mode\":\"eco\"}}");
  TEST_ASSERT_EQUAL(1, resp.n);
  TEST_ASSERT_EQUAL(Result::Ok, resp.r);
  TEST_ASSERT_EQUAL_STRING("1.0", resp.fw.c_str());
  TEST_ASSERT_EQUAL_STRING("eco", resp.mode.c_str());
}

static void test_request_timeout_and_validation() {
  AttrRig r;
  resp = ResponseLog();
  r.connect();
  TEST_ASSERT_EQUAL(Result::Rejected, r.shared.request(nullptr, nullptr, onResponse, nullptr));
  TEST_ASSERT_EQUAL(Result::Queued, r.shared.request(nullptr, "mode", onResponse, nullptr, 300));
  r.clock.advance(300);
  r.c.loop();
  TEST_ASSERT_EQUAL(1, resp.n);
  TEST_ASSERT_EQUAL(Result::Timeout, resp.r);
}

static void test_update_handler_capacity() {
  SharedAttributes<1> s;
  TEST_ASSERT_EQUAL(Result::Ok, s.onUpdate("a", onA));
  TEST_ASSERT_EQUAL(Result::NoMemory, s.onUpdate("b", onA));
  TEST_ASSERT_EQUAL(Result::Rejected, s.onUpdate("a", nullptr));
}

namespace {
int changes = 0;
void onChanged(const char*, void*) { ++changes; }
}  // namespace

static void test_bind_refreshes_on_online() {
  AttrRig r;
  int32_t interval = 0;
  bool enabled = false;
  float gain = 0;
  char mode[8] = "";
  TEST_ASSERT_EQUAL(Result::Ok, r.shared.bind("interval", &interval));
  TEST_ASSERT_EQUAL(Result::Ok, r.shared.bind("enabled", &enabled));
  TEST_ASSERT_EQUAL(Result::Ok, r.shared.bind("gain", &gain));
  TEST_ASSERT_EQUAL(Result::Ok, r.shared.bind("mode", mode, sizeof(mode)));
  TEST_ASSERT_EQUAL(Result::NoMemory, r.shared.onUpdate(nullptr, onA));
  r.connect();
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(1, p.size());
  TEST_ASSERT_EQUAL_STRING("{\"sharedKeys\":\"interval,enabled,gain,mode\"}", p[0].payload.c_str());
  std::string topic = "v1/devices/me/attributes/response/" + requestId(p[0]);
  r.t.message(topic.c_str(), "{\"shared\":{\"interval\":5000,\"enabled\":true,\"gain\":1.5,\"mode\":\"eco\"}}");
  TEST_ASSERT_EQUAL_INT32(5000, interval);
  TEST_ASSERT_TRUE(enabled);
  TEST_ASSERT_TRUE(fabs(gain - 1.5f) < 1e-6);
  TEST_ASSERT_EQUAL_STRING("eco", mode);
}

static void test_bind_update_delete_and_change_callback() {
  AttrRig r;
  changes = 0;
  int32_t interval = 7;
  char mode[4] = "abc";
  r.shared.bind("interval", &interval, onChanged);
  r.shared.bind("mode", mode, sizeof(mode), onChanged);
  r.connect();
  r.update("{\"interval\":250}");
  TEST_ASSERT_EQUAL_INT32(250, interval);
  TEST_ASSERT_EQUAL(1, changes);
  r.update("{\"interval\":250}");  // same value: no change callback
  TEST_ASSERT_EQUAL(1, changes);
  r.update("{\"interval\":2147483648}");  // out of int32 range: ignored
  TEST_ASSERT_EQUAL_INT32(250, interval);
  r.update("{\"interval\":\"x\",\"mode\":\"toolong\"}");  // wrong type, does not fit
  TEST_ASSERT_EQUAL_INT32(250, interval);
  TEST_ASSERT_EQUAL_STRING("abc", mode);
  TEST_ASSERT_EQUAL(1, changes);
  r.update("{\"deleted\":[\"interval\",\"mode\"]}");
  TEST_ASSERT_EQUAL_INT32(0, interval);
  TEST_ASSERT_EQUAL_STRING("", mode);
  TEST_ASSERT_EQUAL(3, changes);
}

static void test_bind_validation() {
  SharedAttributes<2> s;
  int32_t v = 0;
  TEST_ASSERT_EQUAL(Result::Rejected, s.bind(nullptr, &v));
  TEST_ASSERT_EQUAL(Result::Rejected, s.bind("k", static_cast<int32_t*>(nullptr)));
  TEST_ASSERT_EQUAL(Result::Rejected, s.bind("k", static_cast<char*>(nullptr), 4));
  std::string longKey(VL_ATTR_KEYS_SIZE, 'k');
  TEST_ASSERT_EQUAL(Result::Rejected, s.bind(longKey.c_str(), &v));
}

static void test_report_on_change_with_min_interval() {
  AttrRig r;
  int32_t door = 0;
  bool on = false;
  float gain = 1.5f;
  TEST_ASSERT_EQUAL(Result::Ok, r.attrs.report("door", &door, 1000));
  TEST_ASSERT_EQUAL(Result::Ok, r.attrs.report("on", &on));
  TEST_ASSERT_EQUAL(Result::Ok, r.attrs.report("gain", &gain));
  r.connect();
  std::vector<Published> p = r.flush();
  TEST_ASSERT_EQUAL_UINT(3, p.size());
  TEST_ASSERT_EQUAL_STRING("v1/devices/me/attributes", p[0].topic.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"door\":0}", p[0].payload.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"on\":false}", p[1].payload.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"gain\":1.5}", p[2].payload.c_str());
  TEST_ASSERT_EQUAL_UINT(0, r.flush().size());  // unchanged
  door = 1;
  on = true;
  p = r.flush();
  TEST_ASSERT_EQUAL_UINT(1, p.size());  // door waits for its interval
  TEST_ASSERT_EQUAL_STRING("{\"on\":true}", p[0].payload.c_str());
  r.clock.advance(999);
  TEST_ASSERT_EQUAL_UINT(0, r.flush().size());
  r.clock.advance(1);
  p = r.flush();
  TEST_ASSERT_EQUAL_UINT(1, p.size());
  TEST_ASSERT_EQUAL_STRING("{\"door\":1}", p[0].payload.c_str());
}

static void test_report_capacity_and_validation() {
  ClientAttributes<1> a;
  int32_t v = 0;
  TEST_ASSERT_EQUAL(Result::Rejected, a.report(nullptr, &v));
  TEST_ASSERT_EQUAL(Result::Rejected, a.report("k", static_cast<const int32_t*>(nullptr)));
  TEST_ASSERT_EQUAL(Result::Ok, a.report("k", &v));
  TEST_ASSERT_EQUAL(Result::NoMemory, a.report("j", &v));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_subscribes_attribute_topics);
  RUN_TEST(test_update_handlers_per_key_and_deletion);
  RUN_TEST(test_request_and_response);
  RUN_TEST(test_request_timeout_and_validation);
  RUN_TEST(test_update_handler_capacity);
  RUN_TEST(test_bind_refreshes_on_online);
  RUN_TEST(test_bind_update_delete_and_change_callback);
  RUN_TEST(test_bind_validation);
  RUN_TEST(test_report_on_change_with_min_interval);
  RUN_TEST(test_report_capacity_and_validation);
  return UNITY_END();
}
