#include "vl/core/Session.h"

#include <string.h>

#include "vl/Log.h"

namespace vl {

const char* toString(State s) {
  switch (s) {
    case State::Idle: return "Idle";
    case State::Connecting: return "Connecting";
    case State::Subscribing: return "Subscribing";
    case State::Online: return "Online";
    case State::Backoff: return "Backoff";
    case State::AuthFailed: return "AuthFailed";
  }
  return "?";
}

Session::Session(IMqttTransport& transport, const Router& router)
    : transport_(transport), router_(router), backoff_(timing_.backoffBaseMs, timing_.backoffCapMs) {}

void Session::setTiming(const Timing& t) {
  timing_ = t;
  backoff_ = Backoff(t.backoffBaseMs, t.backoffCapMs);
}

void Session::enter(State s) {
  if (s == state_) return;
  State from = state_;
  state_ = s;
  VL_LOGI("session: %s -> %s", toString(from), toString(s));
  if (listener_) listener_->onSessionState(from, s);
}

void Session::start(const ConnectOptions& opts, uint64_t nowMs) {
  if (state_ != State::Idle) transport_.disconnect();
  opts_ = opts;
  backoff_.reset();
  connectNow(nowMs);
}

void Session::stop() {
  if (state_ == State::Idle) return;
  transport_.disconnect();
  bool hadLink = state_ == State::Subscribing || state_ == State::Online;
  enter(State::Idle);
  if (hadLink && listener_) listener_->onSessionLinkLost();
}

void Session::reconnectNow(uint64_t nowMs) {
  if (state_ == State::Backoff || state_ == State::AuthFailed) {
    backoff_.reset();
    connectNow(nowMs);
  }
}

void Session::connectNow(uint64_t nowMs) {
  ++connects_;
  deadline_ = nowMs + timing_.connectTimeoutMs;
  enter(State::Connecting);
  transport_.beginConnect(opts_);
}

void Session::fail(uint64_t nowMs, uint32_t minDelayMs) {
  bool hadLink = state_ == State::Subscribing || state_ == State::Online;
  transport_.disconnect();
  uint32_t delay = backoff_.next(minDelayMs);
  deadline_ = nowMs + delay;
  VL_LOGW("session: retry in %lu ms", static_cast<unsigned long>(delay));
  enter(State::Backoff);
  if (hadLink && listener_) listener_->onSessionLinkLost();
}

void Session::tick(uint64_t nowMs) {
  switch (state_) {
    case State::Connecting:
      if (nowMs >= deadline_) {
        VL_LOGW("session: no CONNACK within %lu ms", static_cast<unsigned long>(timing_.connectTimeoutMs));
        fail(nowMs, 0);
      }
      break;
    case State::Subscribing:
      if (nowMs >= deadline_) {
        VL_LOGW("session: SUBACK timeout");
        fail(nowMs, 0);
      } else if (!transport_.connected()) {
        fail(nowMs, 0);
      }
      break;
    case State::Online:
      // Safety net in case the transport lost its closed event.
      if (!transport_.connected()) fail(nowMs, 0);
      break;
    case State::Backoff:
    case State::AuthFailed:
      if (nowMs >= deadline_) connectNow(nowMs);
      break;
    case State::Idle:
      break;
  }
}

void Session::connack(uint8_t rc, uint64_t nowMs) {
  if (state_ != State::Connecting) return;
  switch (rc) {
    case 0: {
      size_t n = router_.filterCount();
      if (n > 32) n = 32;
      pendingSubacks_ = n == 32 ? 0xFFFFFFFFu : (1u << n) - 1u;
      deadline_ = nowMs + timing_.subscribeTimeoutMs;
      enter(State::Subscribing);
      for (size_t i = 0; i < n; ++i) transport_.subscribe(router_.filter(i), 1);
      if (pendingSubacks_ == 0) {
        backoff_.reset();
        enter(State::Online);
      }
      break;
    }
    case 2:  // identifier rejected
    case 4:  // bad username or password
    case 5:  // not authorized
      VL_LOGE("session: CONNACK %u, credentials rejected", rc);
      transport_.disconnect();
      deadline_ = nowMs + timing_.authRetryMs;
      enter(State::AuthFailed);
      if (listener_) listener_->onSessionAuthFailed(rc);
      break;
    case 3:  // server unavailable (also ThingsBoard's connection rate limit)
      VL_LOGW("session: CONNACK 3, server unavailable");
      fail(nowMs, timing_.serverUnavailableMinMs);
      break;
    default:
      VL_LOGW("session: CONNACK %u", rc);
      fail(nowMs, 0);
      break;
  }
}

void Session::suback(const char* filter, bool granted, uint64_t nowMs) {
  if (state_ != State::Subscribing || !filter) return;
  size_t n = router_.filterCount();
  for (size_t i = 0; i < n && i < 32; ++i) {
    if (strcmp(router_.filter(i), filter) != 0) continue;
    if (!granted) {
      VL_LOGE("session: subscription to %s refused", filter);
      fail(nowMs, 0);
      return;
    }
    pendingSubacks_ &= ~(1u << i);
    break;
  }
  if (pendingSubacks_ == 0) {
    backoff_.reset();
    enter(State::Online);
  }
}

void Session::closed(uint64_t nowMs) {
  if (state_ == State::Connecting || state_ == State::Subscribing || state_ == State::Online) {
    VL_LOGW("session: link closed in %s", toString(state_));
    fail(nowMs, 0);
  }
}

}  // namespace vl
