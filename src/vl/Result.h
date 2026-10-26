#pragma once

#include <stdint.h>

namespace vl {

enum class Result : uint8_t {
  Ok,
  Queued,     // accepted into the outbox, delivery confirmed later by PUBACK
  TooLarge,   // payload exceeds the largest allowed publish
  QueueFull,  // outbox has no room (data records were dropped and counted)
  NotReady,   // client not started or feature not attached
  Timeout,
  Rejected,   // invalid argument or refused by the server
  NoMemory,   // a fixed-capacity table is full
};

inline bool ok(Result r) { return r == Result::Ok || r == Result::Queued; }

const char* toString(Result r);

}  // namespace vl
