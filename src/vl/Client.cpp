#include "vl/Client.h"

#include <string.h>

#include "vl/Log.h"
#include "vl/core/Codec.h"
#include "vl/pal/Platform.h"

#if VL_PLATFORM_ESP32
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace vl {

namespace {

bool copyStr(char* dst, size_t cap, const char* src) {
  if (!src) {
    dst[0] = '\0';
    return true;
  }
  size_t n = strlen(src);
  if (n >= cap) return false;
  memcpy(dst, src, n + 1);
  return true;
}

const char* orNull(const char* s) { return s[0] ? s : nullptr; }

}  // namespace

Client::Client(IMqttTransport& transport, Outbox& outbox)
    : transport_(transport),
      outbox_(outbox),
      clock_(&platformClock()),
      lock_(&ownLock_),
      session_(transport, router_) {
  session_.setListener(this);
  transport_.onConnack(thunkConnack, this);
  transport_.onSuback(thunkSuback, this);
  transport_.onPuback(thunkPuback, this);
  transport_.onMessage(thunkMessage, this);
  transport_.onClosed(thunkClosed, this);
}

Client::~Client() {
  end();
  // A plain IApiModule may already be destroyed; only self-detaching modules
  // are known to be alive here.
  for (size_t i = router_.moduleCount(); i-- > 0;) {
    if (router_.detachesItself(i)) detach(router_.module(i));
  }
}

ApiModule::~ApiModule() {
  if (client_) client_->detach(*this);
}

Result Client::attach(IApiModule& m) {
  Result r = router_.add(m);
  if (r == Result::Ok) m.attach(*this);
  return r;
}

void Client::detach(IApiModule& m) {
  if (exclusive_ == &m) exclusive_ = nullptr;
  {
    LockGuard g(*lock_);
    pending_.removeOwner(&m);
  }
  router_.remove(m);
  m.onDetach();
}

uint16_t Client::bootId() {
  if (!bootIdSet_) {
    bootId_ = static_cast<uint16_t>(platformRandom());
    bootIdSet_ = true;
  }
  return bootId_;
}

Result Client::copyCredentials(const Credentials& auth) {
  if (!copyStr(username_, sizeof(username_), auth.username) || !copyStr(password_, sizeof(password_), auth.password)) {
    return Result::TooLarge;
  }
  const char* id = auth.kind == Credentials::Kind::MqttBasic ? auth.clientId : cfg_.clientId;
  if (id && id[0]) {
    if (!copyStr(clientId_, sizeof(clientId_), id)) {
      VL_LOGE("client: client ID longer than 23 characters");
      return Result::Rejected;
    }
  } else if (!clientId_[0]) {
    platformClientId(clientId_, sizeof(clientId_));
  }
  return Result::Ok;
}

Result Client::begin(const Config& cfg) { return beginSession(cfg, nullptr); }

Result Client::beginExclusive(const Config& cfg, IApiModule& m) {
  bool attached = false;
  for (size_t i = 0; i < router_.moduleCount(); ++i) attached |= &router_.module(i) == &m;
  if (!attached) return Result::NotReady;
  return beginSession(cfg, &m);
}

Result Client::beginSession(const Config& cfg, IApiModule* exclusive) {
  if (!cfg.host || !cfg.host[0] || cfg.maxInFlight == 0) return Result::Rejected;
  if (cfg.auth.kind == Credentials::Kind::None) return Result::Rejected;
  if (cfg.auth.kind == Credentials::Kind::X509 && !cfg.tls) return Result::Rejected;
  cfg_ = cfg;
  cfg_.clientId = nullptr;  // copied below; never keep caller pointers
  cfg_.auth = Credentials();
  if (!copyStr(host_, sizeof(host_), cfg.host)) return Result::TooLarge;
  clientId_[0] = '\0';
  if (cfg.clientId && !copyStr(clientId_, sizeof(clientId_), cfg.clientId)) return Result::Rejected;
  Result r = copyCredentials(cfg.auth);
  if (r != Result::Ok) return r;

  size_t packet = transport_.maxPacket();
  size_t limit = packet < sizeof(txBuf_) ? packet : sizeof(txBuf_);
  {
    LockGuard g(*lock_);
    outbox_.setMaxPayload(limit > kEnvelopeOverhead ? limit - kEnvelopeOverhead : 0);
  }
  exclusive_ = exclusive;
  router_.setSolo(exclusive);
  bucket_.configure(cfg.ratePerSec, cfg.rateBurst);
  session_.setTiming(cfg.timing);
  session_.seed(platformRandom());
  bootId();
  begun_ = true;

  ConnectOptions o;
  o.host = host_;
  o.port = cfg.port;
  o.clientId = clientId_;
  o.username = orNull(username_);
  o.password = orNull(password_);
  o.keepAliveS = cfg.keepAliveS;
  o.tls = cfg.tls;
  session_.start(o, clock_->uptimeMs());
  return Result::Ok;
}

Result Client::setCredentials(const Credentials& auth) {
  if (!begun_) return Result::NotReady;
  Result r = copyCredentials(auth);
  if (r != Result::Ok) return r;
  exclusive_ = nullptr;
  router_.setSolo(nullptr);
  ConnectOptions o;
  o.host = host_;
  o.port = cfg_.port;
  o.clientId = clientId_;
  o.username = orNull(username_);
  o.password = orNull(password_);
  o.keepAliveS = cfg_.keepAliveS;
  o.tls = cfg_.tls;
  session_.start(o, clock_->uptimeMs());
  return Result::Ok;
}

void Client::end() {
#if VL_PLATFORM_ESP32
  if (task_) {
    vTaskDelete(static_cast<TaskHandle_t>(task_));
    task_ = nullptr;
  }
#endif
  if (!begun_) return;
  session_.stop();
  begun_ = false;
  // Fail every outstanding request so callers are never left waiting.
  Correlator::Callback cb;
  void* ctx;
  for (;;) {
    bool got;
    {
      LockGuard g(*lock_);
      got = pending_.takeAny(cb, ctx);
    }
    if (!got) break;
    if (cb) cb(Result::NotReady, nullptr, 0, ctx);
  }
}

void Client::loop() {
  transport_.loop();  // fires the thunks below on this task
  uint64_t now = clock_->uptimeMs();
  session_.tick(now);
  syncClock(now);
  if (online()) drain(now);

  Correlator::Callback cb;
  void* ctx;
  for (;;) {
    bool got;
    {
      LockGuard g(*lock_);
      got = pending_.takeExpired(now, cb, ctx);
    }
    if (!got) break;
    if (cb) cb(Result::Timeout, nullptr, 0, ctx);
  }

  Scheduler::Task fn;
  for (;;) {
    bool got;
    {
      LockGuard g(*lock_);
      got = scheduler_.takeDue(now, fn, ctx);
    }
    if (!got) break;
    fn(ctx);
  }

  for (size_t i = 0; i < router_.moduleCount(); ++i) router_.module(i).tick(now);
}

void Client::syncClock(uint64_t nowMs) {
  if (utcSynced_) return;
  uint64_t utc;
  if (!clock_->utcMs(utc)) return;
  utcSynced_ = true;
  size_t n;
  {
    LockGuard g(*lock_);
    n = outbox_.stampUtc(bootId(), nowMs, utc);
  }
  VL_LOGI("client: clock synced, %u queued records stamped", static_cast<unsigned>(n));
}

void Client::drain(uint64_t nowMs) {
  uint64_t start = nowMs;
  for (uint8_t sent = 0; sent < cfg_.drainMaxRecords; ++sent) {
    uint64_t now = clock_->uptimeMs();
    if (now - start >= cfg_.drainMaxMs) break;

    RecordView rec;
    size_t len = 0;
    size_t topicLen = 0;
    {
      LockGuard g(*lock_);
      if (outbox_.inFlight() >= cfg_.maxInFlight) return;
      if (!outbox_.beginSend(rec)) return;
      if (!bucket_.tryTake(now)) {
        outbox_.markUnsent(rec.seq);
        return;
      }
      bool hasTs = false;
      uint64_t ts = 0;
      if (rec.meta.timeKind == TimeKind::Utc) {
        hasTs = true;
        ts = rec.meta.time;
      } else if (rec.meta.timeKind == TimeKind::Uptime && rec.meta.bootId == bootId_ && utcSynced_ &&
                 rec.meta.time <= now) {
        // Enqueued by another task while the clock was being synced.
        uint64_t utc;
        if (clock_->utcMs(utc)) {
          hasTs = true;
          ts = utc - (now - rec.meta.time);
        }
      }
      const char* dev = nullptr;
      if (rec.meta.dev != kNoDevice) dev = deviceName(rec.meta.dev);
      len = codec::encodeRecord(rec, hasTs, ts, dev, txBuf_, sizeof(txBuf_));
      topicLen = formatTopic(rec.meta.topic, rec.meta.arg, rec.meta.arg2, topicBuf_, sizeof(topicBuf_));
      if (len == 0 || topicLen == 0) {
        VL_LOGE("client: record seq=%lu cannot be encoded, discarded", static_cast<unsigned long>(rec.seq));
        outbox_.discard(rec.seq);
        continue;
      }
    }
    // Network I/O happens with no lock held.
    uint16_t packetId = 0;
    bool ok = transport_.publish(topicBuf_, reinterpret_cast<const uint8_t*>(txBuf_), len, 1, &packetId);
    LockGuard g(*lock_);
    if (!ok) {
      outbox_.markUnsent(rec.seq);
      return;
    }
    outbox_.markSent(rec.seq, packetId, now);
  }
}

Result Client::every(uint32_t intervalMs, Scheduler::Task fn, void* ctx) {
  uint64_t now = clock_->uptimeMs();
  LockGuard g(*lock_);
  return scheduler_.every(intervalMs, fn, ctx, now);
}

RecordMeta Client::stampedMeta(OutTopic topic, uint16_t dev) {
  RecordMeta m;
  m.topic = topic;
  m.dev = dev;
  uint64_t utc;
  if (clock_->utcMs(utc)) {
    m.timeKind = TimeKind::Utc;
    m.time = utc;
  } else {
    m.timeKind = TimeKind::Uptime;
    m.time = clock_->uptimeMs();
    m.bootId = bootId();
  }
  return m;
}

Result Client::enqueue(const RecordMeta& meta, const uint8_t* payload, size_t len) {
  Result r;
  {
    LockGuard g(*lock_);
    r = outbox_.push(meta, payload, len);
  }
  if (r == Result::QueueFull) VL_LOGW("client: outbox full, record dropped");
  return r;
}

Result enqueueValues(Client& c, OutTopic topic, uint16_t dev, bool stamp, bool hasUtc, uint64_t utcMs,
                     const char* json, size_t len) {
  RecordMeta m;
  if (hasUtc) {
    m.topic = topic;
    m.dev = dev;
    m.timeKind = TimeKind::Utc;
    m.time = utcMs;
  } else if (stamp) {
    m = c.stampedMeta(topic, dev);
  } else {
    m.topic = topic;
    m.dev = dev;
  }
  return c.enqueue(m, reinterpret_cast<const uint8_t*>(json), len);
}

Result Client::enqueueJson(OutTopic topic, bool stamp, const char* json) {
  if (!json || json[0] != '{') return Result::Rejected;
  return enqueueValues(*this, topic, kNoDevice, stamp, false, 0, json, strlen(json));
}

Result Client::sendTelemetryJson(const char* json) { return enqueueJson(OutTopic::Telemetry, true, json); }

Result Client::sendAttributesJson(const char* json) { return enqueueJson(OutTopic::Attributes, false, json); }

Result Client::claim(const char* secretKey, uint32_t durationMs) {
  char buf[128];
  JsonWriter w(buf, sizeof(buf));
  codec::claim(w, secretKey, durationMs);
  if (w.overflowed()) return Result::TooLarge;
  RecordMeta m;
  m.topic = OutTopic::Claim;
  m.cls = RecordClass::Control;
  m.transient = true;
  return enqueue(m, reinterpret_cast<const uint8_t*>(buf), w.size());
}

uint32_t Client::addPending(uint32_t timeoutMs, Correlator::Callback cb, void* ctx, const IApiModule* owner) {
  uint64_t now = clock_->uptimeMs();
  LockGuard g(*lock_);
  return pending_.add(now + timeoutMs, cb, ctx, owner);
}

bool Client::resolvePending(uint32_t id, const uint8_t* p, size_t n) {
  Correlator::Callback cb;
  void* ctx;
  bool got;
  {
    LockGuard g(*lock_);
    got = pending_.take(id, cb, ctx);
  }
  if (!got) return false;
  if (cb) cb(Result::Ok, p, n, ctx);
  return true;
}

bool Client::cancelPending(uint32_t id) {
  Correlator::Callback cb;
  void* ctx;
  LockGuard g(*lock_);
  return pending_.take(id, cb, ctx);
}

Result Client::publishNow(OutTopic topic, uint32_t arg, PayloadFn fn, void* ctx) {
  if (!exclusive_ || !session_.online()) return Result::NotReady;
  // The drain, the only other user of these buffers, is off in this session.
  JsonWriter w(txBuf_, sizeof(txBuf_));
  fn(w, ctx);
  if (w.overflowed()) return Result::TooLarge;
  if (formatTopic(topic, arg, 0, topicBuf_, sizeof(topicBuf_)) == 0) return Result::Rejected;
  uint16_t packetId = 0;
  bool sent = transport_.publish(topicBuf_, reinterpret_cast<const uint8_t*>(txBuf_), w.size(), 1, &packetId);
  return sent ? Result::Ok : Result::NotReady;
}

Stats Client::stats() {
  LockGuard g(*lock_);
  Stats s;
  s.queuedRecords = outbox_.records();
  s.queuedBytes = outbox_.bytesUsed();
  s.inFlight = outbox_.inFlight();
  s.dropped = outbox_.dropped();
  s.deadLetters = outbox_.deadLetters();
  s.connects = session_.connects();
  return s;
}

// ---- Session listener ----

void Client::onSessionState(State from, State to) {
  (void)from;
  if (to == State::Online) {
    if (exclusive_) {
      exclusive_->onOnline();
    } else {
      for (size_t i = 0; i < router_.moduleCount(); ++i) router_.module(i).onOnline();
    }
  }
  if (stateCb_) stateCb_(to, stateCtx_);
}

void Client::onSessionAuthFailed(uint8_t returnCode) {
  (void)returnCode;
  if (authCb_) authCb_(authCtx_);
}

void Client::onSessionLinkLost() {
  uint64_t now = clock_->uptimeMs();
  LockGuard g(*lock_);
  outbox_.onSessionClosed(now);
}

// ---- Transport callbacks (client task) ----

void Client::thunkConnack(uint8_t rc, void* ctx) {
  Client* c = static_cast<Client*>(ctx);
  c->session_.connack(rc, c->clock_->uptimeMs());
}

void Client::thunkSuback(const char* filter, bool granted, void* ctx) {
  Client* c = static_cast<Client*>(ctx);
  c->session_.suback(filter, granted, c->clock_->uptimeMs());
}

void Client::thunkPuback(uint16_t packetId, void* ctx) {
  Client* c = static_cast<Client*>(ctx);
  LockGuard g(*c->lock_);
  c->outbox_.ack(packetId);
}

void Client::thunkMessage(const char* topic, const uint8_t* p, size_t n, void* ctx) {
  Client* c = static_cast<Client*>(ctx);
  c->router_.dispatch(topic, Bytes(p, n));
}

void Client::thunkClosed(void* ctx) {
  Client* c = static_cast<Client*>(ctx);
  c->session_.closed(c->clock_->uptimeMs());
}

#if VL_PLATFORM_ESP32
namespace {
void clientTask(void* arg) {
  Client* c = static_cast<Client*>(arg);
  for (;;) {
    c->loop();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
}  // namespace

bool Client::startTask(int core, unsigned priority, uint32_t stackBytes) {
  if (task_) return true;
  TaskHandle_t h = nullptr;
  BaseType_t ok = xTaskCreatePinnedToCore(clientTask, "vl_client", stackBytes, this, priority, &h,
                                          core < 0 ? tskNO_AFFINITY : core);
  if (ok != pdPASS) return false;
  task_ = h;
  return true;
}
#endif

}  // namespace vl
