// Golden-string tests for every payload and topic the library produces.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include <string>

#include "vl/Config.h"
#include "vl/core/Codec.h"
#include "vl/core/JsonWriter.h"
#include "vl/core/Topics.h"

using namespace vl;

void setUp() {}
void tearDown() {}

static void test_json_writer_object() {
  char buf[128];
  JsonWriter w(buf, sizeof(buf));
  w.beginObject().key("a").value(1).key("b").value(true).key("s").value("x\"y\n").key("n").null();
  w.key("arr").beginArray().value(1).value(-2).endArray().endObject();
  TEST_ASSERT_TRUE(w.complete());
  TEST_ASSERT_EQUAL_STRING("{\"a\":1,\"b\":true,\"s\":\"x\\\"y\\n\",\"n\":null,\"arr\":[1,-2]}", buf);
}

static void test_json_writer_numbers() {
  char buf[160];
  JsonWriter w(buf, sizeof(buf));
  int32_t i32 = -2147483647 - 1;
  uint64_t u64 = 18446744073709551615ull;
  int64_t i64 = -9223372036854775807ll - 1;
  w.beginArray().value(i32).value(u64).value(i64).value(21.5).value(0.1f).value(1.0 / 0.0).endArray();
  TEST_ASSERT_EQUAL_STRING("[-2147483648,18446744073709551615,-9223372036854775808,21.5,0.100000001,null]", buf);
}

static void test_json_writer_overflow_is_sticky_and_terminated() {
  char buf[8];
  JsonWriter w(buf, sizeof(buf));
  w.beginObject().key("long_key").value(1).endObject();
  TEST_ASSERT_TRUE(w.overflowed());
  TEST_ASSERT_FALSE(w.complete());
  TEST_ASSERT_TRUE(strlen(buf) < sizeof(buf));
}

static void test_json_writer_control_chars_escaped() {
  char buf[32];
  JsonWriter w(buf, sizeof(buf));
  w.value("\x01");
  TEST_ASSERT_EQUAL_STRING("\"\\u0001\"", buf);
}

static std::string topic(OutTopic t, uint32_t a = 0, uint32_t b = 0) {
  char buf[96];
  size_t n = formatTopic(t, a, b, buf, sizeof(buf));
  return std::string(buf, n);
}

static void test_out_topics() {
  TEST_ASSERT_EQUAL_STRING("v1/devices/me/telemetry", topic(OutTopic::Telemetry).c_str());
  TEST_ASSERT_EQUAL_STRING("v1/devices/me/attributes", topic(OutTopic::Attributes).c_str());
  TEST_ASSERT_EQUAL_STRING("v1/devices/me/attributes/request/7", topic(OutTopic::AttrRequest, 7).c_str());
  TEST_ASSERT_EQUAL_STRING("v1/devices/me/rpc/response/42", topic(OutTopic::RpcResponse, 42).c_str());
  TEST_ASSERT_EQUAL_STRING("v1/devices/me/rpc/request/3", topic(OutTopic::RpcRequest, 3).c_str());
  TEST_ASSERT_EQUAL_STRING("v2/fw/request/5/chunk/12", topic(OutTopic::FwChunkRequest, 5, 12).c_str());
  TEST_ASSERT_EQUAL_STRING("/provision/request", topic(OutTopic::ProvisionRequest).c_str());
  TEST_ASSERT_EQUAL_STRING("v1/gateway/connect", topic(OutTopic::GwConnect).c_str());
  TEST_ASSERT_EQUAL_STRING("v1/gateway/attributes/request", topic(OutTopic::GwAttrRequest).c_str());
  char tiny[8];
  TEST_ASSERT_EQUAL_UINT(0, formatTopic(OutTopic::Telemetry, 0, 0, tiny, sizeof(tiny)));
}

static void test_in_topics() {
  Topic t = parseTopic("v1/devices/me/rpc/request/15");
  TEST_ASSERT_EQUAL(InTopic::RpcRequest, t.kind);
  TEST_ASSERT_EQUAL_UINT32(15, t.id);
  t = parseTopic("v2/fw/response/3/chunk/9");
  TEST_ASSERT_EQUAL(InTopic::FwChunk, t.kind);
  TEST_ASSERT_EQUAL_UINT32(3, t.id);
  TEST_ASSERT_EQUAL_UINT32(9, t.id2);
  TEST_ASSERT_EQUAL(InTopic::AttrUpdate, parseTopic("v1/devices/me/attributes").kind);
  TEST_ASSERT_EQUAL(InTopic::AttrResponse, parseTopic("v1/devices/me/attributes/response/1").kind);
  TEST_ASSERT_EQUAL(InTopic::GwRpc, parseTopic("v1/gateway/rpc").kind);
  TEST_ASSERT_EQUAL(InTopic::ProvisionResponse, parseTopic("/provision/response").kind);
  TEST_ASSERT_EQUAL(InTopic::Unknown, parseTopic("v1/devices/me/rpc/request/").kind);
  TEST_ASSERT_EQUAL(InTopic::Unknown, parseTopic("v1/devices/me/rpc/request/1x").kind);
  TEST_ASSERT_EQUAL(InTopic::Unknown, parseTopic("v2/fw/response/3/chunk/").kind);
  TEST_ASSERT_EQUAL(InTopic::Unknown, parseTopic("v1/devices/me/attributesX").kind);
  TEST_ASSERT_EQUAL_STRING("v2/fw/response/+/chunk/+", filterFor(InTopic::FwChunk));
}

static RecordView record(OutTopic t, const char* payload) {
  RecordView r;
  r.meta.topic = t;
  r.payload = reinterpret_cast<const uint8_t*>(payload);
  r.len = strlen(payload);
  return r;
}

static std::string encode(const RecordView& r, bool hasTs, uint64_t ts, const char* dev) {
  char buf[256];
  size_t n = codec::encodeRecord(r, hasTs, ts, dev, buf, sizeof(buf));
  return std::string(buf, n);
}

static void test_encode_telemetry() {
  RecordView r = record(OutTopic::Telemetry, "{\"t\":1}");
  TEST_ASSERT_EQUAL_STRING("{\"t\":1}", encode(r, false, 0, nullptr).c_str());
  TEST_ASSERT_EQUAL_STRING("{\"ts\":1767225600123,\"values\":{\"t\":1}}",
                           encode(r, true, 1767225600123ull, nullptr).c_str());
}

static void test_encode_gateway() {
  RecordView r = record(OutTopic::GwTelemetry, "{\"kwh\":3.5}");
  TEST_ASSERT_EQUAL_STRING("{\"Meter-12\":[{\"kwh\":3.5}]}", encode(r, false, 0, "Meter-12").c_str());
  TEST_ASSERT_EQUAL_STRING("{\"Meter-12\":[{\"ts\":5,\"values\":{\"kwh\":3.5}}]}",
                           encode(r, true, 5, "Meter-12").c_str());
  r = record(OutTopic::GwAttributes, "{\"fw\":\"1.0\"}");
  TEST_ASSERT_EQUAL_STRING("{\"Meter-12\":{\"fw\":\"1.0\"}}", encode(r, false, 0, "Meter-12").c_str());
  r = record(OutTopic::GwClaim, "{\"secretKey\":\"k\",\"durationMs\":60000}");
  TEST_ASSERT_EQUAL_STRING("{\"Dev\":{\"secretKey\":\"k\",\"durationMs\":60000}}", encode(r, false, 0, "Dev").c_str());
  // No device name: cannot be encoded.
  TEST_ASSERT_EQUAL_UINT(0, encode(r, false, 0, nullptr).size());
}

static void test_encode_attributes_never_get_ts_envelope() {
  RecordView r = record(OutTopic::Attributes, "{\"a\":1}");
  TEST_ASSERT_EQUAL_STRING("{\"a\":1}", encode(r, true, 5, nullptr).c_str());
}

static void test_payload_encoders() {
  char buf[160];
  {
    JsonWriter w(buf, sizeof(buf));
    codec::attributeRequest(w, "a,b", "c");
    TEST_ASSERT_EQUAL_STRING("{\"clientKeys\":\"a,b\",\"sharedKeys\":\"c\"}", buf);
  }
  {
    JsonWriter w(buf, sizeof(buf));
    codec::attributeRequest(w, nullptr, "c");
    TEST_ASSERT_EQUAL_STRING("{\"sharedKeys\":\"c\"}", buf);
  }
  {
    JsonWriter w(buf, sizeof(buf));
    codec::claim(w, "secret", 30000);
    TEST_ASSERT_EQUAL_STRING("{\"secretKey\":\"secret\",\"durationMs\":30000}", buf);
  }
  {
    JsonWriter w(buf, sizeof(buf));
    codec::gwConnect(w, "Meter-12", "energy-meter");
    TEST_ASSERT_EQUAL_STRING("{\"device\":\"Meter-12\",\"type\":\"energy-meter\"}", buf);
  }
  {
    JsonWriter w(buf, sizeof(buf));
    codec::gwDisconnect(w, "Meter-12");
    TEST_ASSERT_EQUAL_STRING("{\"device\":\"Meter-12\"}", buf);
  }
  {
    JsonWriter w(buf, sizeof(buf));
    codec::gwAttributeRequest(w, 4, "Meter-12", true, "fw,model");
    TEST_ASSERT_EQUAL_STRING("{\"id\":4,\"device\":\"Meter-12\",\"client\":true,\"keys\":[\"fw\",\"model\"]}", buf);
  }
  {
    JsonWriter w(buf, sizeof(buf));
    codec::gwRpcReply(w, "Meter-12", 9, "{\"ok\":true}", 11);
    TEST_ASSERT_EQUAL_STRING("{\"device\":\"Meter-12\",\"id\":9,\"data\":{\"ok\":true}}", buf);
  }
}

static void test_rpc_payloads() {
  char buf[96];
  {
    JsonWriter w(buf, sizeof(buf));
    codec::rpcError(w, "unknown method: x");
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"unknown method: x\"}", buf);
  }
  {
    JsonWriter w(buf, sizeof(buf));
    codec::rpcRequest(w, "getTime", "{\"tz\":1}", 8);
    TEST_ASSERT_EQUAL_STRING("{\"method\":\"getTime\",\"params\":{\"tz\":1}}", buf);
  }
  {
    JsonWriter w(buf, sizeof(buf));
    codec::rpcRequest(w, "ping", nullptr, 0);
    TEST_ASSERT_EQUAL_STRING("{\"method\":\"ping\",\"params\":{}}", buf);
  }
}

static void test_ota_payloads() {
  char buf[128];
  {
    JsonWriter w(buf, sizeof(buf));
    codec::fwInfo(w, "ViraGate", "2.3.0");
    TEST_ASSERT_EQUAL_STRING("{\"current_fw_title\":\"ViraGate\",\"current_fw_version\":\"2.3.0\"}", buf);
  }
  {
    JsonWriter w(buf, sizeof(buf));
    codec::fwState(w, "DOWNLOADING", nullptr);
    TEST_ASSERT_EQUAL_STRING("{\"fw_state\":\"DOWNLOADING\"}", buf);
  }
  {
    JsonWriter w(buf, sizeof(buf));
    codec::fwState(w, "FAILED", "checksum mismatch");
    TEST_ASSERT_EQUAL_STRING("{\"fw_state\":\"FAILED\",\"fw_error\":\"checksum mismatch\"}", buf);
  }
}

static void test_provision_requests() {
  char buf[256];
  {
    JsonWriter w(buf, sizeof(buf));
    codec::provisionRequest(w, "Meter-7", "KEY", "SECRET", Credentials(), nullptr);
    TEST_ASSERT_EQUAL_STRING(
        "{\"deviceName\":\"Meter-7\",\"provisionDeviceKey\":\"KEY\",\"provisionDeviceSecret\":\"SECRET\"}", buf);
  }
  {
    JsonWriter w(buf, sizeof(buf));
    codec::provisionRequest(w, nullptr, "KEY", "SECRET", AccessToken("tok"), nullptr);
    TEST_ASSERT_EQUAL_STRING(
        "{\"provisionDeviceKey\":\"KEY\",\"provisionDeviceSecret\":\"SECRET\",\"credentialsType\":\"ACCESS_TOKEN\","
        "\"token\":\"tok\"}",
        buf);
  }
  {
    JsonWriter w(buf, sizeof(buf));
    codec::provisionRequest(w, "M", "K", "S", MqttBasic("cid", "user", "pass"), nullptr);
    TEST_ASSERT_EQUAL_STRING(
        "{\"deviceName\":\"M\",\"provisionDeviceKey\":\"K\",\"provisionDeviceSecret\":\"S\",\"credentialsType\":"
        "\"MQTT_BASIC\",\"username\":\"user\",\"password\":\"pass\",\"clientId\":\"cid\"}",
        buf);
  }
  {
    JsonWriter w(buf, sizeof(buf));
    codec::provisionRequest(w, "M", "K", "S", X509(), "-----BEGIN CERTIFICATE-----\nMIIB\n");
    TEST_ASSERT_EQUAL_STRING(
        "{\"deviceName\":\"M\",\"provisionDeviceKey\":\"K\",\"provisionDeviceSecret\":\"S\",\"credentialsType\":"
        "\"X509_CERTIFICATE\",\"hash\":\"-----BEGIN CERTIFICATE-----\\nMIIB\\n\"}",
        buf);
  }
}

static std::string fmt(double v, int digits) {
  char buf[32];
  size_t n = json::formatDouble(v, digits, buf);
  return std::string(buf, n);
}

static HealthSample fullHealth() {
  HealthSample s;
  s.keys = kHealthAll;
  s.uptimeS = 3600;
  s.heapFree = 181234;
  s.heapMin = 150112;
  s.resetReason = "power_on";
  s.rssi = -61;
  s.iface = "wifi";
  s.queueDepth = 3;
  s.dropped = 1;
  s.deadLetters = 2;
  s.sdk = "2.0.0";
  return s;
}

static void test_health_payloads() {
  char buf[VL_HEALTH_SIZE];
  {
    JsonWriter w(buf, sizeof(buf));
    codec::health(w, fullHealth(), false);
    TEST_ASSERT_EQUAL_STRING(
        "{\"vl_uptime\":3600,\"vl_heap_free\":181234,\"vl_heap_min\":150112,\"vl_reset_reason\":\"power_on\","
        "\"vl_rssi\":-61,\"vl_iface\":\"wifi\",\"vl_queue_depth\":3,\"vl_dropped\":1,\"vl_dead_letter\":2,"
        "\"vl_sdk\":\"2.0.0\"}",
        buf);
  }
  {
    // 1.0.2 names where 1.0.2 had the key; vl_ names elsewhere.
    JsonWriter w(buf, sizeof(buf));
    codec::health(w, fullHealth(), true);
    TEST_ASSERT_EQUAL_STRING(
        "{\"upTime\":3600,\"ESP Free Heap\":181234,\"vl_heap_min\":150112,\"vl_reset_reason\":\"power_on\","
        "\"vl_rssi\":-61,\"vl_iface\":\"wifi\",\"vl_queue_depth\":3,\"vl_dropped\":1,\"vl_dead_letter\":2,"
        "\"Viralink SDK Version\":\"2.0.0\"}",
        buf);
  }
  {
    HealthSample s = fullHealth();
    s.keys = kHealthUptime | kHealthRssi | kHealthSdk;
    JsonWriter w(buf, sizeof(buf));
    codec::health(w, s, false);
    TEST_ASSERT_EQUAL_STRING("{\"vl_uptime\":3600,\"vl_rssi\":-61,\"vl_sdk\":\"2.0.0\"}", buf);
  }
  {
    // A key with no value (unknown on this platform) is left out even when selected.
    HealthSample s = fullHealth();
    s.resetReason = nullptr;
    s.iface = nullptr;
    s.sdk = nullptr;
    s.keys = kHealthResetReason | kHealthIface | kHealthSdk | kHealthDropped;
    JsonWriter w(buf, sizeof(buf));
    codec::health(w, s, false);
    TEST_ASSERT_EQUAL_STRING("{\"vl_dropped\":1}", buf);
  }
}

static void test_health_longest_payload_fits() {
  HealthSample s = fullHealth();
  s.uptimeS = s.heapFree = s.heapMin = s.queueDepth = s.dropped = s.deadLetters = 4294967295u;
  s.resetReason = "deep_sleep";
  s.rssi = -32768;
  s.iface = "cellular";
  s.sdk = VL_SDK_VERSION;
  char buf[VL_HEALTH_SIZE];
  for (int v1 = 0; v1 < 2; ++v1) {
    JsonWriter w(buf, sizeof(buf));
    codec::health(w, s, v1 != 0);
    TEST_ASSERT_TRUE(w.complete());
  }
}

static void test_format_double() {
  TEST_ASSERT_EQUAL_STRING("21.5", fmt(21.5, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("0.100000001", fmt(0.1f, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("0.1", fmt(0.1f, 7).c_str());
  TEST_ASSERT_EQUAL_STRING("-3.25", fmt(-3.25, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("100", fmt(100.0, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("0", fmt(0.0, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("0", fmt(-0.0, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("123456789", fmt(123456789.0, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("1.23456789e+09", fmt(1234567890.0, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("1.234568e+09", fmt(1234567890.0, 7).c_str());
  TEST_ASSERT_EQUAL_STRING("0.0001", fmt(0.0001, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("1e-05", fmt(0.00001, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("10", fmt(9.9999999999, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("3.40282347e+38", fmt(3.4028234663852886e38, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("3.402823e+38", fmt(3.4028234663852886e38, 7).c_str());
  TEST_ASSERT_EQUAL_STRING("1e-300", fmt(1e-300, 9).c_str());
  TEST_ASSERT_EQUAL_STRING("1.79769313e+308", fmt(1.7976931348623157e308, 9).c_str());
}

// Nine digits bring every float back unchanged, as printf("%.9g") does.
static void test_format_double_round_trips_floats() {
  uint32_t x = 12345;
  for (int i = 0; i < 20000; ++i) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    float f;
    memcpy(&f, &x, sizeof(f));
    if (isnan(f) || isinf(f)) continue;
    std::string s = fmt(f, 9);
    TEST_ASSERT_TRUE_MESSAGE(static_cast<float>(strtod(s.c_str(), nullptr)) == f, s.c_str());
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_json_writer_object);
  RUN_TEST(test_json_writer_numbers);
  RUN_TEST(test_json_writer_overflow_is_sticky_and_terminated);
  RUN_TEST(test_json_writer_control_chars_escaped);
  RUN_TEST(test_out_topics);
  RUN_TEST(test_in_topics);
  RUN_TEST(test_encode_telemetry);
  RUN_TEST(test_encode_gateway);
  RUN_TEST(test_encode_attributes_never_get_ts_envelope);
  RUN_TEST(test_payload_encoders);
  RUN_TEST(test_rpc_payloads);
  RUN_TEST(test_ota_payloads);
  RUN_TEST(test_provision_requests);
  RUN_TEST(test_health_payloads);
  RUN_TEST(test_health_longest_payload_fits);
  RUN_TEST(test_format_double);
  RUN_TEST(test_format_double_round_trips_floats);
  return UNITY_END();
}
