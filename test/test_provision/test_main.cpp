// Provisioning: saved credentials, the exclusive provisioning session, every
// credentials type, refusals, timeouts and reconnects.
#include <string.h>
#include <unity.h>

#include <algorithm>
#include <string>
#include <vector>

#include "Fakes.h"
#include "vl/Client.h"
#include "vl/api/Provision.h"
#include "vl/api/Rpc.h"

using namespace vl;
using vltest::FakeClock;
using vltest::FakeTransport;
using vltest::MemoryStore;
using vltest::Published;

void setUp() {}
void tearDown() {}

namespace {

struct Done {
  int n = 0;
  Result r = Result::Ok;
  std::string error;
} done;

void onDone(Result r, const char* error, void*) {
  ++done.n;
  done.r = r;
  done.error = error ? error : "";
}

const char* const kOkToken = "{\"credentialsType\":\"ACCESS_TOKEN\",\"credentialsValue\":\"NEWTOKEN\",\"status\":\"SUCCESS\"}";

std::string blob(uint8_t kind, const char* cid, const char* user, const char* pass) {
  std::string b("V\x01", 2);
  b += static_cast<char>(kind);
  b += cid;
  b += '\0';
  b += user;
  b += '\0';
  b += pass;
  b += '\0';
  return b;
}

bool has(const std::vector<std::string>& v, const char* s) { return std::find(v.begin(), v.end(), s) != v.end(); }

struct ProvRig {
  FakeTransport t;
  FakeClock clock;
  RingOutbox<4096> outbox;
  Client c;
  Config cfg;
  MemoryStore store;
  Provisioning prov;
  Rpc<1> rpc;

  ProvRig() : c(t, outbox), prov(&store) {
    done = Done();
    c.setClock(clock);
    cfg.host = "console.viralink.ir";
    cfg.ratePerSec = 0;
    c.use(rpc);
    prov.onDone(onDone, nullptr);
    TEST_ASSERT_EQUAL(Result::Ok, prov.setDevice("Meter-7", "KEY", "SECRET"));
  }
  // Starts provisioning up to the request; returns the request payload.
  std::string request() {
    TEST_ASSERT_EQUAL(Result::Ok, prov.start(c, cfg));
    c.loop();
    TEST_ASSERT_EQUAL_STRING("provision", t.lastUser.c_str());
    t.connack(0);  // no SUBACK comes for /provision/response
    TEST_ASSERT_EQUAL(State::Online, c.state());
    TEST_ASSERT_FALSE(c.online());
    TEST_ASSERT_EQUAL_UINT(1, t.subscribed.size());
    TEST_ASSERT_EQUAL_STRING("/provision/response", t.subscribed[0].c_str());
    t.subscribed.clear();
    t.published.clear();
    c.loop();
    TEST_ASSERT_EQUAL_UINT(1, t.published.size());
    TEST_ASSERT_EQUAL_STRING("/provision/request", t.published[0].topic.c_str());
    return t.published[0].payload;
  }
  // Delivers the response; the normal session starts on the next loop.
  void respond(const char* json) {
    t.message("/provision/response", json);
    t.published.clear();
    c.loop();
  }
  void online() {
    t.connack(0);
    TEST_ASSERT_FALSE(has(t.subscribed, "/provision/response"));
    TEST_ASSERT_TRUE(has(t.subscribed, "v1/devices/me/rpc/request/+"));
    t.subackAll();
    TEST_ASSERT_TRUE(c.online());
  }
};

}  // namespace

static void test_saved_credentials_skip_provisioning() {
  {
    ProvRig r;
    r.store.data["vl_cred"] = blob(1, "", "SAVED", "");
    TEST_ASSERT_EQUAL(Result::Ok, r.prov.start(r.c, r.cfg));
    r.c.loop();
    TEST_ASSERT_EQUAL_STRING("SAVED", r.t.lastUser.c_str());
    r.online();
    TEST_ASSERT_TRUE(r.prov.credentials().kind == Credentials::Kind::AccessToken);
  }
  {
    ProvRig r;
    r.store.data["vl_cred"] = blob(2, "cid", "user", "pass");
    TEST_ASSERT_EQUAL(Result::Ok, r.prov.start(r.c, r.cfg));
    r.c.loop();
    TEST_ASSERT_EQUAL_STRING("cid", r.t.lastOpts.clientId);
    TEST_ASSERT_EQUAL_STRING("user", r.t.lastUser.c_str());
    TEST_ASSERT_EQUAL_STRING("pass", r.t.lastOpts.password);
  }
  // Unreadable or foreign blobs are ignored: the device provisions instead.
  const std::string bad[] = {"junk", blob(9, "", "x", ""), blob(1, "", "", ""), std::string("V\x01\x01x", 4)};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    ProvRig r;
    r.store.data["vl_cred"] = bad[i];
    r.request();
  }
}

static void test_provisions_with_server_generated_token() {
  ProvRig r;
  TEST_ASSERT_EQUAL(Result::Queued, r.c.telemetry().add("t", 1).send());  // waits for the real session
  std::string req = r.request();
  TEST_ASSERT_EQUAL_STRING(
      "{\"deviceName\":\"Meter-7\",\"provisionDeviceKey\":\"KEY\",\"provisionDeviceSecret\":\"SECRET\"}", req.c_str());
  r.t.published.clear();
  r.c.loop();
  TEST_ASSERT_EQUAL_UINT(0, r.t.published.size());  // nothing else in this session

  int connects = r.t.connectCalls;
  r.respond(kOkToken);
  TEST_ASSERT_EQUAL(1, done.n);
  TEST_ASSERT_EQUAL(Result::Ok, done.r);
  TEST_ASSERT_EQUAL(connects + 1, r.t.connectCalls);
  TEST_ASSERT_EQUAL_STRING("NEWTOKEN", r.t.lastUser.c_str());
  TEST_ASSERT_TRUE(r.store.data["vl_cred"] == blob(1, "", "NEWTOKEN", ""));
  TEST_ASSERT_EQUAL_STRING("NEWTOKEN", r.prov.credentials().username);

  r.online();
  r.t.published.clear();
  r.c.loop();
  TEST_ASSERT_EQUAL_UINT(1, r.t.published.size());
  TEST_ASSERT_EQUAL_STRING("v1/devices/me/telemetry", r.t.published[0].topic.c_str());

  // A later boot uses the saved token.
  ProvRig next;
  next.store.data = r.store.data;
  TEST_ASSERT_EQUAL(Result::Ok, next.prov.start(next.c, next.cfg));
  next.c.loop();
  TEST_ASSERT_EQUAL_STRING("NEWTOKEN", next.t.lastUser.c_str());
}

static void test_device_chosen_credentials() {
  {
    ProvRig r;
    TEST_ASSERT_EQUAL(Result::Ok, r.prov.setCredentials(AccessToken("MYTOKEN")));
    std::string req = r.request();
    TEST_ASSERT_TRUE(req.find("\"credentialsType\":\"ACCESS_TOKEN\",\"token\":\"MYTOKEN\"") != std::string::npos);
    r.respond("{\"credentialsType\":\"ACCESS_TOKEN\",\"credentialsValue\":\"MYTOKEN\",\"status\":\"SUCCESS\"}");
    TEST_ASSERT_EQUAL_STRING("MYTOKEN", r.t.lastUser.c_str());
  }
  {
    ProvRig r;
    TEST_ASSERT_EQUAL(Result::Ok, r.prov.setCredentials(MqttBasic("cid", "user", "pass")));
    std::string req = r.request();
    TEST_ASSERT_TRUE(req.find("\"credentialsType\":\"MQTT_BASIC\",\"username\":\"user\",\"password\":\"pass\","
                              "\"clientId\":\"cid\"") != std::string::npos);
    r.respond(
        "{\"credentialsType\":\"MQTT_BASIC\",\"credentialsValue\":{\"clientId\":\"cid\",\"userName\":\"user\","
        "\"password\":\"pass\"},\"status\":\"SUCCESS\"}");
    TEST_ASSERT_EQUAL(Result::Ok, done.r);
    TEST_ASSERT_EQUAL_STRING("cid", r.t.lastOpts.clientId);
    TEST_ASSERT_EQUAL_STRING("user", r.t.lastUser.c_str());
    TEST_ASSERT_EQUAL_STRING("pass", r.t.lastOpts.password);
    TEST_ASSERT_TRUE(r.store.data["vl_cred"] == blob(2, "cid", "user", "pass"));
  }
  {
    ProvRig r;
    TEST_ASSERT_EQUAL(Result::Rejected, r.prov.setCredentials(AccessToken("")));
    TEST_ASSERT_EQUAL(Result::Rejected, r.prov.setCredentials(MqttBasic(nullptr, nullptr, "pass")));
    std::string longToken(VL_USERNAME_SIZE, 'x');
    TEST_ASSERT_EQUAL(Result::TooLarge, r.prov.setCredentials(AccessToken(longToken.c_str())));
  }
}

static void test_x509() {
  PemTls tls("CA", "CERT", "KEY");
  const char* pem = "-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n";
  {
    ProvRig r;
    TEST_ASSERT_EQUAL(Result::Ok, r.prov.setCredentials(X509()));
    TEST_ASSERT_EQUAL(Result::Rejected, r.prov.start(r.c, r.cfg));  // no certificate
    r.prov.setCertificate(pem);
    TEST_ASSERT_EQUAL(Result::Rejected, r.prov.start(r.c, r.cfg));  // no Config::tls
  }
  ProvRig r;
  r.cfg.tls = &tls;
  r.prov.setCredentials(X509());
  r.prov.setCertificate(pem);
  std::string req = r.request();
  TEST_ASSERT_TRUE(req.find("\"credentialsType\":\"X509_CERTIFICATE\",\"hash\":\"-----BEGIN CERTIFICATE-----\\nMIIB") !=
                   std::string::npos);
  // Older ThingsBoard documentation spells the status key this way.
  r.respond("{\"credentialsType\":\"X509_CERTIFICATE\",\"credentialsValue\":\"MIIB\","
            "\"provisionDeviceStatus\":\"SUCCESS\"}");
  TEST_ASSERT_EQUAL(Result::Ok, done.r);
  TEST_ASSERT_EQUAL_STRING("", r.t.lastUser.c_str());
  TEST_ASSERT_TRUE(r.t.lastOpts.tls == &tls);
  TEST_ASSERT_TRUE(r.prov.credentials().kind == Credentials::Kind::X509);
}

static void test_refused_or_invalid_response_stops_the_client() {
  {
    ProvRig r;
    r.request();
    r.respond("{\"errorMsg\":\"Provision data was not found!\",\"status\":\"NOT_FOUND\"}");
    TEST_ASSERT_EQUAL(1, done.n);
    TEST_ASSERT_EQUAL(Result::Rejected, done.r);
    TEST_ASSERT_EQUAL_STRING("Provision data was not found!", done.error.c_str());
    TEST_ASSERT_EQUAL(State::Idle, r.c.state());
    TEST_ASSERT_TRUE(r.store.data.empty());
    // A second response after the end is ignored.
    r.respond(kOkToken);
    TEST_ASSERT_EQUAL(1, done.n);
  }
  const char* invalid[] = {
      "{\"credentialsType\":\"LWM2M_CREDENTIALS\",\"status\":\"SUCCESS\"}",
      "{\"credentialsType\":\"ACCESS_TOKEN\",\"credentialsValue\":\"\",\"status\":\"SUCCESS\"}",
      "{\"credentialsType\":\"MQTT_BASIC\",\"credentialsValue\":{\"password\":\"p\"},\"status\":\"SUCCESS\"}",
      "not json",
  };
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    ProvRig r;
    r.request();
    r.respond(invalid[i]);
    TEST_ASSERT_EQUAL_MESSAGE(Result::Rejected, done.r, invalid[i]);
    TEST_ASSERT_EQUAL(State::Idle, r.c.state());
  }
}

static void test_times_out() {
  ProvRig r;
  r.request();
  r.clock.advance(VL_PROVISION_TIMEOUT_MS);
  r.c.loop();
  TEST_ASSERT_EQUAL(1, done.n);
  TEST_ASSERT_EQUAL(Result::Timeout, done.r);
  TEST_ASSERT_EQUAL(State::Idle, r.c.state());
  // It can be started again.
  r.t.subscribed.clear();
  r.request();
}

static void test_request_sent_again_after_a_reconnect() {
  ProvRig r;
  r.request();
  r.t.close();
  r.clock.advance(2000);
  r.c.loop();
  TEST_ASSERT_EQUAL_STRING("provision", r.t.lastUser.c_str());
  r.t.connack(0);
  r.t.published.clear();
  r.c.loop();
  TEST_ASSERT_EQUAL_UINT(1, r.t.published.size());
  TEST_ASSERT_EQUAL_STRING("/provision/request", r.t.published[0].topic.c_str());
  r.respond(kOkToken);
  TEST_ASSERT_EQUAL(Result::Ok, done.r);
}

static void test_setup_errors_and_forget() {
  ProvRig r;
  Provisioning bare(&r.store);
  TEST_ASSERT_EQUAL(Result::Rejected, bare.start(r.c, r.cfg));  // no setDevice()
  TEST_ASSERT_EQUAL(Result::Rejected, bare.setDevice("x", "", "S"));
  std::string longKey(VL_PROVISION_KEY_SIZE, 'k');
  TEST_ASSERT_EQUAL(Result::TooLarge, bare.setDevice("x", longKey.c_str(), "S"));
  // Without a name the server picks one.
  TEST_ASSERT_EQUAL(Result::Ok, r.prov.setDevice(nullptr, "KEY", "SECRET"));
  r.store.data["vl_cred"] = blob(1, "", "SAVED", "");
  TEST_ASSERT_EQUAL(Result::Ok, r.prov.forget());
  TEST_ASSERT_TRUE(r.store.data.empty());
  std::string req = r.request();
  TEST_ASSERT_EQUAL_STRING("{\"provisionDeviceKey\":\"KEY\",\"provisionDeviceSecret\":\"SECRET\"}", req.c_str());
}

static void test_unsaved_credentials_still_connect() {
  ProvRig r;
  r.store.failSet = true;
  r.request();
  r.respond(kOkToken);
  TEST_ASSERT_EQUAL(Result::Ok, done.r);
  TEST_ASSERT_EQUAL_STRING("NEWTOKEN", r.t.lastUser.c_str());
  TEST_ASSERT_TRUE(r.store.data.empty());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_saved_credentials_skip_provisioning);
  RUN_TEST(test_provisions_with_server_generated_token);
  RUN_TEST(test_device_chosen_credentials);
  RUN_TEST(test_x509);
  RUN_TEST(test_refused_or_invalid_response_stops_the_client);
  RUN_TEST(test_times_out);
  RUN_TEST(test_request_sent_again_after_a_reconnect);
  RUN_TEST(test_setup_errors_and_forget);
  RUN_TEST(test_unsaved_credentials_still_connect);
  return UNITY_END();
}
