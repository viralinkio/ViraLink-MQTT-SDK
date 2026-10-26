#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vl/Config.h"
#include "vl/Credentials.h"
#include "vl/IApiModule.h"
#include "vl/Payload.h"
#include "vl/Result.h"
#include "vl/core/Correlator.h"
#include "vl/core/Outbox.h"
#include "vl/core/RateControl.h"
#include "vl/core/Router.h"
#include "vl/core/Scheduler.h"
#include "vl/core/Session.h"
#include "vl/pal/Clock.h"
#include "vl/pal/Lock.h"
#include "vl/transport/IMqttTransport.h"

namespace vl {

// Plain struct (C++11). Strings are copied by begin().
struct Config {
  const char* host = nullptr;
  uint16_t port = 1883;
  Credentials auth;
  const char* clientId = nullptr;  // default: "vl-" + MAC; at most 23 characters
  uint16_t keepAliveS = 60;
  const ITlsProvider* tls = nullptr;  // must outlive the client

  uint8_t maxInFlight = 4;        // QoS1 records awaiting PUBACK
  uint16_t ratePerSec = 10;       // drain rate limit; 0 = unlimited
  uint16_t rateBurst = 10;
  uint8_t drainMaxRecords = 8;    // per loop()
  uint16_t drainMaxMs = 20;       // per loop()
  Session::Timing timing;
};

struct Stats {
  size_t queuedRecords;
  size_t queuedBytes;
  size_t inFlight;
  uint32_t dropped;
  uint32_t deadLetters;
  uint32_t connects;
};

class Client : private SessionListener {
 public:
  typedef void (*StateCb)(State s, void* ctx);
  typedef void (*AuthFailedCb)(void* ctx);
  typedef const char* (*DeviceNameFn)(uint16_t dev, void* ctx);

  Client(IMqttTransport& transport, Outbox& outbox);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Platform overrides (tests, custom ports). Call before begin().
  void setClock(IClock& clock) { clock_ = &clock; }
  void setLock(ILock& lock) { lock_ = &lock; }

  // Attaches API modules. Call before begin(): the subscription registry is
  // built from the attached modules.
  Result use() { return Result::Ok; }
  template <class M, class... Rest>
  Result use(M& m, Rest&... rest) {
    Result r = attach(m);
    return r == Result::Ok ? use(rest...) : r;
  }
  Result attach(IApiModule& m);
  // Removes a module: its pending requests are dropped without firing. Call
  // on the client task or while loop() is not running; ApiModule calls it
  // from its destructor.
  void detach(IApiModule& m);

  void onState(StateCb cb, void* ctx) {
    stateCb_ = cb;
    stateCtx_ = ctx;
  }
  void onAuthFailed(AuthFailedCb cb, void* ctx) {
    authCb_ = cb;
    authCtx_ = ctx;
  }

  Result begin(const Config& cfg);
  // Replaces the credentials and reconnects immediately.
  Result setCredentials(const Credentials& auth);
  void end();
  // Drives the session, drains the outbox, fires timeouts and scheduled
  // tasks. Call often from one task (not needed after startTask()).
  void loop();
#if VL_PLATFORM_ESP32
  // Runs loop() on a dedicated FreeRTOS task.
  bool startTask(int core = 0, unsigned priority = 5, uint32_t stackBytes = 6144);
#endif

  State state() const { return session_.state(); }
  // False during a provisioning session, which only its module may use.
  bool online() const { return session_.online() && !exclusive_; }

  // Periodic task on the client task, timed on uptime. Thread-safe.
  Result every(uint32_t intervalMs, Scheduler::Task fn, void* ctx);

  // Builders: client.telemetry().add("t", 21.5).send(). Thread-safe.
  template <size_t N = VL_BUILDER_SIZE>
  PayloadBuilder<N> telemetry() {
    return PayloadBuilder<N>(*this, OutTopic::Telemetry, kNoDevice, true);
  }
  template <size_t N = VL_BUILDER_SIZE>
  PayloadBuilder<N> attributes() {
    return PayloadBuilder<N>(*this, OutTopic::Attributes, kNoDevice, false);
  }
  // Pre-encoded values object, e.g. "{\"temp\":21.5}".
  Result sendTelemetryJson(const char* json);
  Result sendAttributesJson(const char* json);
  // Lets a user claim this device with secretKey (null = none) within
  // durationMs. Thread-safe.
  Result claim(const char* secretKey, uint32_t durationMs);

  // ---- Module API (thread-safe) ----
  Result enqueue(const RecordMeta& meta, const uint8_t* payload, size_t len);
  RecordMeta stampedMeta(OutTopic topic, uint16_t dev);
  // Registers a pending request; returns its id, or 0 when the table is full.
  // owner's requests are dropped, unfired, when it detaches.
  uint32_t addPending(uint32_t timeoutMs, Correlator::Callback cb, void* ctx, const IApiModule* owner = nullptr);
  // Delivers a response; returns false if the id is unknown or expired.
  bool resolvePending(uint32_t id, const uint8_t* p, size_t n);
  // Drops a pending request without firing it (its publish was refused).
  bool cancelPending(uint32_t id);
  // Guards the outbox and module tables: hold it only around table access,
  // never across network I/O or a callback.
  ILock& lock() { return *lock_; }
  // The queue itself, for vl::Persistence; access it only under lock().
  Outbox& outbox() { return outbox_; }
  // Exclusive session (vl::Provisioning): like begin(), but only m's topics
  // are subscribed, only m sees Online, online() stays false and nothing
  // leaves the outbox. begin() or setCredentials() returns to the normal
  // session. m must be attached; NotReady otherwise.
  Result beginExclusive(const Config& cfg, IApiModule& m);
  typedef void (*PayloadFn)(JsonWriter& w, void* ctx);
  // Exclusive session only, on the client task: fn writes the payload into the
  // transmit buffer and it is published at once, bypassing the outbox.
  // NotReady when not connected, TooLarge if it did not fit.
  Result publishNow(OutTopic topic, uint32_t arg, PayloadFn fn, void* ctx);
  void setDeviceNames(DeviceNameFn fn, void* ctx) {
    deviceName_ = fn;
    deviceNameCtx_ = ctx;
  }

  uint64_t uptimeMs() { return clock_->uptimeMs(); }
  bool utcMs(uint64_t& out) { return clock_->utcMs(out); }
  uint16_t bootId();
  Stats stats();
  size_t maxPayload() const { return outbox_.maxPayload(); }
  // Largest MQTT packet the transport accepts in either direction.
  size_t maxPacket() const { return transport_.maxPacket(); }
  const ITlsProvider* tls() const { return cfg_.tls; }

 private:
  static const size_t kEnvelopeOverhead = 128;  // {"<device>":[{"ts":…,"values":…}]}

  void onSessionState(State from, State to) override;
  void onSessionAuthFailed(uint8_t returnCode) override;
  void onSessionLinkLost() override;

  static void thunkConnack(uint8_t rc, void* ctx);
  static void thunkSuback(const char* filter, bool granted, void* ctx);
  static void thunkPuback(uint16_t packetId, void* ctx);
  static void thunkMessage(const char* topic, const uint8_t* p, size_t n, void* ctx);
  static void thunkClosed(void* ctx);

  Result beginSession(const Config& cfg, IApiModule* exclusive);
  void drain(uint64_t nowMs);
  void syncClock(uint64_t nowMs);
  Result copyCredentials(const Credentials& auth);
  Result enqueueJson(OutTopic topic, bool stamp, const char* json);
  friend Result enqueueValues(Client& c, OutTopic topic, uint16_t dev, bool stamp, bool hasUtc, uint64_t utcMs,
                              const char* json, size_t len);

  IMqttTransport& transport_;
  Outbox& outbox_;
  IClock* clock_;
  PlatformLock ownLock_;
  ILock* lock_;
  Router router_;
  Session session_;
  Correlator pending_;
  Scheduler scheduler_;
  TokenBucket bucket_;
  Config cfg_;

  StateCb stateCb_ = nullptr;
  void* stateCtx_ = nullptr;
  AuthFailedCb authCb_ = nullptr;
  void* authCtx_ = nullptr;
  DeviceNameFn deviceName_ = nullptr;
  void* deviceNameCtx_ = nullptr;
  IApiModule* exclusive_ = nullptr;

  char host_[64] = {};
  char clientId_[24] = {};
  char username_[VL_USERNAME_SIZE] = {};
  char password_[VL_PASSWORD_SIZE] = {};
  char topicBuf_[96] = {};
  char txBuf_[VL_TX_BUFFER_SIZE];

  uint16_t bootId_ = 0;
  bool bootIdSet_ = false;
  bool utcSynced_ = false;
  bool begun_ = false;
#if VL_PLATFORM_ESP32
  void* task_ = nullptr;
#endif
};

}  // namespace vl
