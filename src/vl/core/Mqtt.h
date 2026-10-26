// MQTT 3.1.1 packet encoding and decoding, for transports that speak MQTT
// themselves (StreamMqtt). Only what a client needs: clean sessions, QoS 0/1,
// one filter per SUBSCRIBE.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vl {
namespace mqtt {

enum Type : uint8_t {
  kConnect = 1,
  kConnack = 2,
  kPublish = 3,
  kPuback = 4,
  kSubscribe = 8,
  kSuback = 9,
  kPingreq = 12,
  kPingresp = 13,
  kDisconnect = 14,
};

// Largest remaining length MQTT can express.
const uint32_t kMaxRemaining = 268435455;

// Writers return the bytes written, or 0 if cap is too small.
// CONNECT with a clean session; user and pass may be null.
size_t connect(uint8_t* out, size_t cap, const char* clientId, const char* user, const char* pass, uint16_t keepAliveS);
// The PUBLISH packet up to the payload, which the caller sends after it.
// packetId is used only for qos 1.
size_t publishHead(uint8_t* out, size_t cap, const char* topic, size_t payloadLen, uint8_t qos, uint16_t packetId);
size_t puback(uint8_t* out, size_t cap, uint16_t packetId);
size_t subscribe(uint8_t* out, size_t cap, uint16_t packetId, const char* filter, uint8_t qos);
size_t pingreq(uint8_t* out, size_t cap);
size_t disconnect(uint8_t* out, size_t cap);

struct Header {
  uint8_t type = 0;
  uint8_t flags = 0;
  uint32_t remaining = 0;  // bytes after the fixed header
  size_t size = 0;         // fixed header bytes
};

enum class Parse : uint8_t { Ok, NeedMore, Malformed };

// Decodes the fixed header at the start of p[0..n).
Parse header(const uint8_t* p, size_t n, Header& h);

// Body parsers; false if the body is malformed.
bool connack(const uint8_t* body, size_t n, uint8_t& returnCode);
// topic is not NUL-terminated; packetId is 0 for qos 0.
bool publish(uint8_t flags, const uint8_t* body, size_t n, const uint8_t*& topic, size_t& topicLen, uint8_t& qos,
             uint16_t& packetId, const uint8_t*& payload, size_t& payloadLen);
bool ack(const uint8_t* body, size_t n, uint16_t& packetId);  // PUBACK
// granted is false for the failure code 0x80.
bool suback(const uint8_t* body, size_t n, uint16_t& packetId, bool& granted);

}  // namespace mqtt
}  // namespace vl
