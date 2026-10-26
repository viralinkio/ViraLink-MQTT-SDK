#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vl/core/RateControl.h"
#include "vl/core/Router.h"
#include "vl/transport/IMqttTransport.h"

namespace vl {

enum class State : uint8_t {
  Idle,         // not started, or stopped
  Connecting,   // waiting for CONNACK
  Subscribing,  // CONNACK ok, waiting for every SUBACK; nothing is published yet
  Online,       // publishing allowed
  Backoff,      // waiting before the next attempt
  AuthFailed,   // CONNACK 2/4/5; slow retry until new credentials are set
};

const char* toString(State s);

class SessionListener {
 public:
  virtual ~SessionListener() {}
  virtual void onSessionState(State from, State to) = 0;
  virtual void onSessionAuthFailed(uint8_t returnCode) = 0;
  // The link dropped after CONNACK; in-flight records must be resent.
  virtual void onSessionLinkLost() = 0;
};

// The one state machine that owns the connection. Driven by transport
// callbacks and tick(); never blocks.
class Session {
 public:
  struct Timing {
    uint32_t connectTimeoutMs = 30000;
    uint32_t subscribeTimeoutMs = 10000;
    uint32_t authRetryMs = 300000;
    uint32_t serverUnavailableMinMs = 30000;  // CONNACK 3, also ThingsBoard's connection rate limit
    uint32_t backoffBaseMs = 1000;
    uint32_t backoffCapMs = 60000;
  };

  Session(IMqttTransport& transport, const Router& router);

  void setListener(SessionListener* l) { listener_ = l; }
  void setTiming(const Timing& t);
  void seed(uint32_t s) { backoff_.seed(s); }

  // Starts connecting with these options; the strings must stay valid until
  // stop() or the next start().
  void start(const ConnectOptions& opts, uint64_t nowMs);
  void stop();
  // Skips any backoff or auth-failure wait (for example after new credentials).
  void reconnectNow(uint64_t nowMs);

  void tick(uint64_t nowMs);
  void connack(uint8_t returnCode, uint64_t nowMs);
  void suback(const char* filter, bool granted, uint64_t nowMs);
  void closed(uint64_t nowMs);

  State state() const { return state_; }
  bool online() const { return state_ == State::Online; }
  uint64_t retryAt() const { return deadline_; }
  uint32_t connects() const { return connects_; }

 private:
  void enter(State s);
  void connectNow(uint64_t nowMs);
  void fail(uint64_t nowMs, uint32_t minDelayMs);

  IMqttTransport& transport_;
  const Router& router_;
  SessionListener* listener_ = nullptr;
  ConnectOptions opts_;
  Timing timing_;
  Backoff backoff_;
  State state_ = State::Idle;
  uint64_t deadline_ = 0;  // timeout or retry time for the current state
  uint32_t pendingSubacks_ = 0;
  uint32_t connects_ = 0;
};

}  // namespace vl
