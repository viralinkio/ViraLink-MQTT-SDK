// JsonValue: validation, lookup, iteration and conversions over spans that
// are not NUL-terminated.
#include <math.h>
#include <string.h>
#include <unity.h>

#include <string>

#include "vl/core/JsonReader.h"

using namespace vl;

void setUp() {}
void tearDown() {}

static JsonValue P(const char* s) { return JsonValue::parse(s, strlen(s)); }

static void test_scalars() {
  TEST_ASSERT_EQUAL(JsonValue::Type::Null, P("null").type());
  TEST_ASSERT_TRUE(P("true").asBool());
  TEST_ASSERT_FALSE(P("false").asBool(true));
  TEST_ASSERT_EQUAL_INT(-42, static_cast<int>(P(" -42 ").asInt()));
  TEST_ASSERT_TRUE(fabs(P("2.15e1").asDouble() - 21.5) < 1e-9);
  TEST_ASSERT_EQUAL_INT(7, static_cast<int>(P("7.9").asInt()));
  TEST_ASSERT_EQUAL_INT(5, static_cast<int>(P("\"x\"").asInt(5)));
  TEST_ASSERT_TRUE(P("1").asBool(true));  // wrong type: default
}

static void test_rejects_invalid() {
  const char* bad[] = {"",     "{",    "{\"a\":}",   "[1,]", "{\"a\" 1}",  "tru",   "01",   "1.",
                       "\"open", "{} x", "{\"a\":1,}", "-",    "\"bad\\q\"", "[1 2]", "{1:2}", "\"a\x01\""};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) TEST_ASSERT_FALSE_MESSAGE(P(bad[i]).valid(), bad[i]);
}

static void test_depth_limit() {
  std::string ok = std::string(VL_JSON_MAX_DEPTH, '[') + std::string(VL_JSON_MAX_DEPTH, ']');
  std::string deep = std::string(VL_JSON_MAX_DEPTH + 1, '[') + std::string(VL_JSON_MAX_DEPTH + 1, ']');
  TEST_ASSERT_TRUE(P(ok.c_str()).valid());
  TEST_ASSERT_FALSE(P(deep.c_str()).valid());
}

static void test_object_lookup_and_iteration() {
  JsonValue v = P("{\"method\":\"setRelay\",\"params\":{\"on\":true,\"ch\":[1,2,3]},\"x\":null}");
  TEST_ASSERT_EQUAL(JsonValue::Type::Object, v.type());
  TEST_ASSERT_EQUAL_UINT(3, v.size());
  TEST_ASSERT_TRUE(v["method"].equals("setRelay"));
  TEST_ASSERT_TRUE(v["params"]["on"].asBool());
  TEST_ASSERT_EQUAL_UINT(3, v["params"]["ch"].size());
  TEST_ASSERT_EQUAL_INT(3, static_cast<int>(v["params"]["ch"].at(2).asInt()));
  TEST_ASSERT_FALSE(v["params"]["ch"].at(3).valid());
  TEST_ASSERT_FALSE(v["missing"].valid());
  TEST_ASSERT_FALSE(v["missing"]["deeper"].valid());
  TEST_ASSERT_EQUAL(JsonValue::Type::Null, v["x"].type());
  JsonValue k, val;
  TEST_ASSERT_TRUE(v.member(1, k, val));
  TEST_ASSERT_TRUE(k.equals("params"));
  TEST_ASSERT_EQUAL(JsonValue::Type::Object, val.type());
  TEST_ASSERT_FALSE(v.member(3, k, val));
  JsonValue ch = v["params"]["ch"];
  TEST_ASSERT_EQUAL_UINT(7, ch.rawSize());
  TEST_ASSERT_EQUAL_MEMORY("[1,2,3]", ch.raw(), 7);
}

static void test_strings_unescape() {
  JsonValue v = P("{\"s\":\"a\\\"b\\\\c\\/d\\n\\u00e9\\ud83d\\ude00\"}");
  char out[32];
  TEST_ASSERT_TRUE(v["s"].copyString(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("a\"b\\c/d\n\xc3\xa9\xf0\x9f\x98\x80", out);
  char small[4];
  TEST_ASSERT_FALSE(v["s"].copyString(small, sizeof(small)));
  TEST_ASSERT_TRUE(P("{\"k\\u0065y\":1}")["key"].valid());
  TEST_ASSERT_FALSE(P("1").copyString(out, sizeof(out)));
  TEST_ASSERT_TRUE(P("\"ab\"").equals("ab"));
  TEST_ASSERT_FALSE(P("\"ab\"").equals("abc"));
  TEST_ASSERT_FALSE(P("\"ab\"").equals("a"));
}

static void test_span_not_nul_terminated() {
  const char buf[] = {'{', '"', 'a', '"', ':', '1', '2', '}', '9'};
  TEST_ASSERT_EQUAL_INT(12, static_cast<int>(JsonValue::parse(buf, 8)["a"].asInt()));
  TEST_ASSERT_EQUAL_INT(12, static_cast<int>(JsonValue::parse("123", 2).asInt()));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_scalars);
  RUN_TEST(test_rejects_invalid);
  RUN_TEST(test_depth_limit);
  RUN_TEST(test_object_lookup_and_iteration);
  RUN_TEST(test_strings_unescape);
  RUN_TEST(test_span_not_nul_terminated);
  return UNITY_END();
}
