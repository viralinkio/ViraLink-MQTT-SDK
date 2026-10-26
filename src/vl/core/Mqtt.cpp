#include "vl/core/Mqtt.h"

#include <string.h>

namespace vl {
namespace mqtt {

namespace {

// n > kMaxRemaining (2^28 - 1), written so that it also compiles where size_t
// is 16 bits without an always-false comparison.
bool tooLong(size_t n) { return ((n >> 14) >> 14) != 0; }

size_t lengthBytes(uint32_t n) { return n < 128 ? 1 : n < 16384 ? 2 : n < 2097152 ? 3 : 4; }

// Writes the fixed header; returns its size, 0 if it or the remaining bytes
// do not fit.
size_t fixedHeader(uint8_t* out, size_t cap, uint8_t first, size_t remaining) {
  if (tooLong(remaining)) return 0;
  size_t n = 1 + lengthBytes(static_cast<uint32_t>(remaining));
  if (cap < n || cap - n < remaining) return 0;
  out[0] = first;
  size_t i = 1;
  do {
    uint8_t b = static_cast<uint8_t>(remaining % 128);
    remaining /= 128;
    out[i++] = remaining ? static_cast<uint8_t>(b | 0x80) : b;
  } while (remaining);
  return n;
}

uint8_t* putU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
  return p + 2;
}

uint8_t* putStr(uint8_t* p, const char* s, size_t n) {
  p = putU16(p, static_cast<uint16_t>(n));
  memcpy(p, s, n);
  return p + n;
}

uint16_t getU16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

size_t len(const char* s) { return s ? strlen(s) : 0; }

}  // namespace

size_t connect(uint8_t* out, size_t cap, const char* clientId, const char* user, const char* pass, uint16_t keepAliveS) {
  size_t idLen = len(clientId), userLen = len(user), passLen = len(pass);
  if (idLen > 0xFFFF || userLen > 0xFFFF || passLen > 0xFFFF || (pass && !user)) return 0;
  size_t remaining = 10 + 2 + idLen + (user ? 2 + userLen : 0) + (pass ? 2 + passLen : 0);
  size_t h = fixedHeader(out, cap, kConnect << 4, remaining);
  if (!h) return 0;
  uint8_t* p = putStr(out + h, "MQTT", 4);
  *p++ = 4;  // protocol level 3.1.1
  *p++ = static_cast<uint8_t>(0x02 | (user ? 0x80 : 0) | (pass ? 0x40 : 0));  // clean session
  p = putU16(p, keepAliveS);
  p = putStr(p, clientId ? clientId : "", idLen);
  if (user) p = putStr(p, user, userLen);
  if (pass) p = putStr(p, pass, passLen);
  return static_cast<size_t>(p - out);
}

size_t publishHead(uint8_t* out, size_t cap, const char* topic, size_t payloadLen, uint8_t qos, uint16_t packetId) {
  size_t topicLen = len(topic);
  if (topicLen == 0 || topicLen > 0xFFFF || qos > 1) return 0;
  size_t head = 2 + topicLen + (qos ? 2 : 0);
  size_t remaining = head + payloadLen;
  if (tooLong(remaining)) return 0;
  size_t h = 1 + lengthBytes(static_cast<uint32_t>(remaining));
  if (cap < h + head) return 0;
  // Only the head is written here: the payload follows separately.
  fixedHeader(out, h + remaining, static_cast<uint8_t>((kPublish << 4) | (qos << 1)), remaining);
  uint8_t* p = putStr(out + h, topic, topicLen);
  if (qos) p = putU16(p, packetId);
  return static_cast<size_t>(p - out);
}

size_t puback(uint8_t* out, size_t cap, uint16_t packetId) {
  size_t h = fixedHeader(out, cap, kPuback << 4, 2);
  if (!h) return 0;
  putU16(out + h, packetId);
  return h + 2;
}

size_t subscribe(uint8_t* out, size_t cap, uint16_t packetId, const char* filter, uint8_t qos) {
  size_t n = len(filter);
  if (n == 0 || n > 0xFFFF || qos > 1) return 0;
  size_t h = fixedHeader(out, cap, (kSubscribe << 4) | 0x02, 2 + 2 + n + 1);
  if (!h) return 0;
  uint8_t* p = putU16(out + h, packetId);
  p = putStr(p, filter, n);
  *p++ = qos;
  return static_cast<size_t>(p - out);
}

size_t pingreq(uint8_t* out, size_t cap) { return fixedHeader(out, cap, kPingreq << 4, 0); }

size_t disconnect(uint8_t* out, size_t cap) { return fixedHeader(out, cap, kDisconnect << 4, 0); }

Parse header(const uint8_t* p, size_t n, Header& h) {
  if (n < 2) return Parse::NeedMore;
  uint8_t type = p[0] >> 4;
  if (type == 0 || type == 15) return Parse::Malformed;
  uint32_t value = 0;
  uint32_t mul = 1;
  for (size_t i = 1; i <= 4; ++i) {
    if (i >= n) return Parse::NeedMore;
    value += (p[i] & 0x7Fu) * mul;
    if (!(p[i] & 0x80)) {
      h.type = type;
      h.flags = p[0] & 0x0F;
      h.remaining = value;
      h.size = i + 1;
      return Parse::Ok;
    }
    mul *= 128;
  }
  return Parse::Malformed;
}

bool connack(const uint8_t* body, size_t n, uint8_t& returnCode) {
  if (n != 2) return false;
  returnCode = body[1];
  return true;
}

bool publish(uint8_t flags, const uint8_t* body, size_t n, const uint8_t*& topic, size_t& topicLen, uint8_t& qos,
             uint16_t& packetId, const uint8_t*& payload, size_t& payloadLen) {
  qos = (flags >> 1) & 0x03;
  if (qos > 2 || n < 2) return false;
  topicLen = getU16(body);
  size_t off = 2 + topicLen;
  if (off > n) return false;
  topic = body + 2;
  packetId = 0;
  if (qos) {
    if (off + 2 > n) return false;
    packetId = getU16(body + off);
    off += 2;
  }
  payload = body + off;
  payloadLen = n - off;
  return true;
}

bool ack(const uint8_t* body, size_t n, uint16_t& packetId) {
  if (n != 2) return false;
  packetId = getU16(body);
  return true;
}

bool suback(const uint8_t* body, size_t n, uint16_t& packetId, bool& granted) {
  if (n != 3) return false;
  packetId = getU16(body);
  granted = body[2] != 0x80;
  return true;
}

}  // namespace mqtt
}  // namespace vl
