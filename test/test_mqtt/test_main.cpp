// MQTT 3.1.1 codec: golden bytes for every packet a client sends, and
// parsing of every packet it receives, including malformed ones.
#include <string.h>
#include <unity.h>

#include <string>
#include <vector>

#include "vl/core/Mqtt.h"

using namespace vl;

void setUp() {}
void tearDown() {}

namespace {

std::vector<uint8_t> bytes(std::initializer_list<int> v) {
  std::vector<uint8_t> out;
  for (int b : v) out.push_back(static_cast<uint8_t>(b));
  return out;
}

std::vector<uint8_t> str(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

std::vector<uint8_t> cat(std::vector<uint8_t> a, const std::vector<uint8_t>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

void assertBytes(const std::vector<uint8_t>& expected, const uint8_t* got, size_t n) {
  TEST_ASSERT_EQUAL_UINT(expected.size(), n);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected.data(), got, n);
}

}  // namespace

static void test_connect() {
  uint8_t buf[128];
  // Token as username, no password, keep-alive 60, clean session.
  size_t n = mqtt::connect(buf, sizeof(buf), "vl-1", "TOKEN", nullptr, 60);
  assertBytes(cat(cat(bytes({0x10, 23, 0, 4, 'M', 'Q', 'T', 'T', 4, 0x82, 0, 60, 0, 4}), str("vl-1")),
                  cat(bytes({0, 5}), str("TOKEN"))),
              buf, n);
  // Username and password; no username at all.
  n = mqtt::connect(buf, sizeof(buf), "c", "u", "p", 30);
  assertBytes(bytes({0x10, 19, 0, 4, 'M', 'Q', 'T', 'T', 4, 0xC2, 0, 30, 0, 1, 'c', 0, 1, 'u', 0, 1, 'p'}), buf, n);
  n = mqtt::connect(buf, sizeof(buf), "c", nullptr, nullptr, 0);
  assertBytes(bytes({0x10, 13, 0, 4, 'M', 'Q', 'T', 'T', 4, 0x02, 0, 0, 0, 1, 'c'}), buf, n);
  TEST_ASSERT_EQUAL_UINT(0, mqtt::connect(buf, 10, "c", "u", "p", 30));
}

static void test_publish_head() {
  uint8_t buf[64];
  size_t n = mqtt::publishHead(buf, sizeof(buf), "a/b", 5, 1, 0x1234);
  assertBytes(bytes({0x32, 12, 0, 3, 'a', '/', 'b', 0x12, 0x34}), buf, n);  // + 5 payload bytes
  n = mqtt::publishHead(buf, sizeof(buf), "t", 2, 0, 99);
  assertBytes(bytes({0x30, 5, 0, 1, 't'}), buf, n);
  // Remaining lengths of 2 and 3 bytes.
  n = mqtt::publishHead(buf, sizeof(buf), "t", 200, 0, 0);
  assertBytes(bytes({0x30, 0xCB, 0x01, 0, 1, 't'}), buf, n);  // 203
  n = mqtt::publishHead(buf, sizeof(buf), "t", 20000, 1, 1);
  assertBytes(bytes({0x32, 0xA5, 0x9C, 0x01, 0, 1, 't', 0, 1}), buf, n);  // 20005
  TEST_ASSERT_EQUAL_UINT(0, mqtt::publishHead(buf, 4, "abc", 1, 0, 0));
}

static void test_small_packets() {
  uint8_t buf[32];
  assertBytes(bytes({0x40, 2, 0x01, 0x02}), buf, mqtt::puback(buf, sizeof(buf), 0x0102));
  assertBytes(cat(bytes({0x82, 11, 0, 7, 0, 6}), cat(str("v1/a/+"), bytes({1}))), buf,
              mqtt::subscribe(buf, sizeof(buf), 7, "v1/a/+", 1));
  assertBytes(bytes({0xC0, 0}), buf, mqtt::pingreq(buf, sizeof(buf)));
  assertBytes(bytes({0xE0, 0}), buf, mqtt::disconnect(buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_UINT(0, mqtt::puback(buf, 3, 1));
}

static void test_header() {
  mqtt::Header h;
  std::vector<uint8_t> p = bytes({0x20, 2, 0, 0});
  TEST_ASSERT_EQUAL(mqtt::Parse::Ok, mqtt::header(p.data(), p.size(), h));
  TEST_ASSERT_EQUAL_UINT8(mqtt::kConnack, h.type);
  TEST_ASSERT_EQUAL_UINT32(2, h.remaining);
  TEST_ASSERT_EQUAL_UINT(2, h.size);

  p = bytes({0x3B, 0xFF, 0xFF, 0xFF, 0x7F});
  TEST_ASSERT_EQUAL(mqtt::Parse::Ok, mqtt::header(p.data(), p.size(), h));
  TEST_ASSERT_EQUAL_UINT8(mqtt::kPublish, h.type);
  TEST_ASSERT_EQUAL_HEX8(0x0B, h.flags);
  TEST_ASSERT_EQUAL_UINT32(mqtt::kMaxRemaining, h.remaining);
  TEST_ASSERT_EQUAL_UINT(5, h.size);

  p = bytes({0x30, 0x80});
  TEST_ASSERT_EQUAL(mqtt::Parse::NeedMore, mqtt::header(p.data(), p.size(), h));
  TEST_ASSERT_EQUAL(mqtt::Parse::NeedMore, mqtt::header(p.data(), 1, h));
  p = bytes({0x30, 0x80, 0x80, 0x80, 0x80, 0x01});  // five length bytes
  TEST_ASSERT_EQUAL(mqtt::Parse::Malformed, mqtt::header(p.data(), p.size(), h));
  p = bytes({0x00, 0x00});  // type 0 is reserved
  TEST_ASSERT_EQUAL(mqtt::Parse::Malformed, mqtt::header(p.data(), p.size(), h));
}

static void test_parse_bodies() {
  uint8_t rc = 0xFF;
  std::vector<uint8_t> b = bytes({0, 5});
  TEST_ASSERT_TRUE(mqtt::connack(b.data(), b.size(), rc));
  TEST_ASSERT_EQUAL_UINT8(5, rc);
  TEST_ASSERT_FALSE(mqtt::connack(b.data(), 1, rc));

  uint16_t id = 0;
  b = bytes({0x12, 0x34});
  TEST_ASSERT_TRUE(mqtt::ack(b.data(), b.size(), id));
  TEST_ASSERT_EQUAL_HEX16(0x1234, id);
  TEST_ASSERT_FALSE(mqtt::ack(b.data(), 1, id));

  bool granted = false;
  b = bytes({0, 7, 1});
  TEST_ASSERT_TRUE(mqtt::suback(b.data(), b.size(), id, granted));
  TEST_ASSERT_EQUAL_UINT16(7, id);
  TEST_ASSERT_TRUE(granted);
  b = bytes({0, 7, 0x80});
  TEST_ASSERT_TRUE(mqtt::suback(b.data(), b.size(), id, granted));
  TEST_ASSERT_FALSE(granted);
  TEST_ASSERT_FALSE(mqtt::suback(b.data(), 2, id, granted));
}

static void test_parse_publish() {
  const uint8_t* topic;
  size_t topicLen;
  uint8_t qos;
  const uint8_t* payload;
  size_t payloadLen;
  uint16_t id;
  std::vector<uint8_t> b = cat(cat(bytes({0, 3}), str("a/b")), cat(bytes({0, 9}), str("{}")));
  TEST_ASSERT_TRUE(mqtt::publish(0x02, b.data(), b.size(), topic, topicLen, qos, id, payload, payloadLen));
  TEST_ASSERT_EQUAL_UINT8(1, qos);
  TEST_ASSERT_EQUAL_UINT16(9, id);
  TEST_ASSERT_EQUAL_STRING_LEN("a/b", topic, 3);
  TEST_ASSERT_EQUAL_UINT(3, topicLen);
  TEST_ASSERT_EQUAL_STRING_LEN("{}", payload, 2);
  TEST_ASSERT_EQUAL_UINT(2, payloadLen);

  b = cat(cat(bytes({0, 1}), str("t")), str("xyz"));  // qos 0: no packet id
  TEST_ASSERT_TRUE(mqtt::publish(0x00, b.data(), b.size(), topic, topicLen, qos, id, payload, payloadLen));
  TEST_ASSERT_EQUAL_UINT8(0, qos);
  TEST_ASSERT_EQUAL_UINT16(0, id);
  TEST_ASSERT_EQUAL_UINT(3, payloadLen);

  b = cat(bytes({0, 1}), str("t"));  // empty payload
  TEST_ASSERT_TRUE(mqtt::publish(0x00, b.data(), b.size(), topic, topicLen, qos, id, payload, payloadLen));
  TEST_ASSERT_EQUAL_UINT(0, payloadLen);

  b = bytes({0, 9, 'a'});  // topic runs past the body
  TEST_ASSERT_FALSE(mqtt::publish(0x00, b.data(), b.size(), topic, topicLen, qos, id, payload, payloadLen));
  b = cat(bytes({0, 1}), str("t"));  // qos 1 without its packet id
  TEST_ASSERT_FALSE(mqtt::publish(0x02, b.data(), b.size(), topic, topicLen, qos, id, payload, payloadLen));
  TEST_ASSERT_FALSE(mqtt::publish(0x06, b.data(), b.size(), topic, topicLen, qos, id, payload, payloadLen));  // qos 3
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_connect);
  RUN_TEST(test_publish_head);
  RUN_TEST(test_small_packets);
  RUN_TEST(test_header);
  RUN_TEST(test_parse_bodies);
  RUN_TEST(test_parse_publish);
  return UNITY_END();
}
