#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "vl/Result.h"
#include "vl/core/JsonWriter.h"
#include "vl/core/Outbox.h"

namespace vl {

class Client;

// Implemented in Client.cpp; keeps this header free of the Client definition.
Result enqueueValues(Client& c, OutTopic topic, uint16_t dev, bool stamp, bool hasUtc, uint64_t utcMs,
                     const char* json, size_t len);

// Builds a values object on the stack and queues it:
//   client.telemetry().add("temp", 21.5).add("on", true).send();
// N is the object's capacity in bytes; send() returns TooLarge if it overflowed.
template <size_t N>
class PayloadBuilder {
 public:
  PayloadBuilder(Client& c, OutTopic topic, uint16_t dev, bool stamp)
      : client_(&c), topic_(topic), dev_(dev), stamp_(stamp), w_(buf_, N) {
    w_.beginObject();
  }

  // A builder whose send() returns failed and queues nothing, e.g. for a
  // sub-device the gateway could not add.
  explicit PayloadBuilder(Result failed)
      : client_(nullptr), topic_(OutTopic::Telemetry), dev_(kNoDevice), stamp_(false), failed_(failed), w_(buf_, N) {
    w_.beginObject();
  }

  PayloadBuilder(const PayloadBuilder& o)
      : client_(o.client_),
        topic_(o.topic_),
        dev_(o.dev_),
        stamp_(o.stamp_),
        hasTs_(o.hasTs_),
        ts_(o.ts_),
        failed_(o.failed_),
        w_(o.w_) {
    memcpy(buf_, o.buf_, N);
    w_.rebind(buf_);
  }
  PayloadBuilder& operator=(const PayloadBuilder&) = delete;

  template <class T>
  PayloadBuilder& add(const char* key, T value) {
    w_.key(key).value(value);
    return *this;
  }

  // Adds an already-encoded JSON value (object, array, number...).
  PayloadBuilder& addJson(const char* key, const char* json) {
    w_.key(key).raw(json, strlen(json));
    return *this;
  }

  // Explicit UTC timestamp in ms. Without it, telemetry is stamped with the
  // enqueue time and converted to UTC once the clock is known.
  PayloadBuilder& ts(uint64_t utcMs) {
    hasTs_ = true;
    ts_ = utcMs;
    return *this;
  }

  Result send() {
    if (!client_) return failed_;
    w_.endObject();
    if (w_.overflowed()) return Result::TooLarge;
    return enqueueValues(*client_, topic_, dev_, stamp_, hasTs_, ts_, w_.c_str(), w_.size());
  }

 private:
  Client* client_;
  OutTopic topic_;
  uint16_t dev_;
  bool stamp_;
  bool hasTs_ = false;
  uint64_t ts_ = 0;
  Result failed_ = Result::Ok;
  char buf_[N];
  JsonWriter w_;
};

}  // namespace vl
