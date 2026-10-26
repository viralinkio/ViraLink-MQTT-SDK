#include "vl/net/Networks.h"

#include "vl/Log.h"

namespace vl {

bool LinkNetwork::up() {
  if (!up_) return false;
  if (checkMs_ == 0) return up_(ctx_);
  uint64_t now = clock_->uptimeMs();
  if (!checked_ || now - lastCheck_ >= checkMs_) {
    last_ = up_(ctx_);
    lastCheck_ = now;
    checked_ = true;
  }
  return last_;
}

NetworkSet::NetworkSet(Slot* slots, size_t cap, IClock* clock)
    : slots_(slots), cap_(cap), clock_(clock ? clock : &platformClock()) {}

Result NetworkSet::add(INetwork& net) {
  if (count_ == cap_) return Result::NoMemory;
  Slot& s = slots_[count_++];
  s.net = &net;
  s.owner = this;
  s.upSince = 0;
  s.penaltyUntil = 0;
  s.up = false;
  // A shared transport ends up with the last slot as its context; events are
  // matched by transport, so any slot of it will do.
  IMqttTransport& t = net.transport();
  t.onConnack(thunkConnack, &s);
  t.onSuback(thunkSuback, &s);
  t.onPuback(thunkPuback, &s);
  t.onMessage(thunkMessage, &s);
  t.onClosed(thunkClosed, &s);
  return Result::Ok;
}

bool NetworkSet::firstWith(size_t i) const {
  for (size_t j = 0; j < i; ++j) {
    if (&slots_[j].net->transport() == &slots_[i].net->transport()) return false;
  }
  return true;
}

bool NetworkSet::fromActive(const Slot* s) const {
  return active_ >= 0 && &s->net->transport() == &transportOf(active_);
}

// The best network that is up, other than except.
int NetworkSet::best(uint64_t now, int except, bool allowPenalised) const {
  for (size_t i = 0; i < count_; ++i) {
    if (static_cast<int>(i) != except && slots_[i].up && (allowPenalised || !penalised(i, now))) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void NetworkSet::evaluate(uint64_t now) {
  anyUp_ = false;
  for (size_t i = 0; i < count_; ++i) {
    Slot& s = slots_[i];
    bool up = s.net->up();
    if (up && !s.up) s.upSince = now;
    s.up = up;
    anyUp_ = anyUp_ || up;
  }

  int target = active_;
  if (active_ < 0 || !slots_[active_].up) {
    target = best(now, -1, false);
    if (target < 0) target = best(now, -1, true);
  } else {
    // The session has been trying (recently) for stallMs with no CONNACK.
    if (trying_ && now - tryingSince_ >= timing_.stallMs && now - lastAttempt_ < timing_.stallMs) {
      int other = best(now, active_, false);
      if (other >= 0) {
        VL_LOGW("networks: no broker reached over %s in %lu s", slots_[active_].net->name(),
                static_cast<unsigned long>(timing_.stallMs / 1000));
        slots_[active_].penaltyUntil = now + timing_.retryMs;
        target = other;
      }
    }
    for (int i = 0; target == active_ && i < active_; ++i) {
      const Slot& s = slots_[i];
      if (s.up && !penalised(static_cast<size_t>(i), now) && now - s.upSince >= timing_.failBackMs) target = i;
    }
  }
  if (target != active_) switchTo(target);
}

void NetworkSet::switchTo(int target) {
  const char* from = active_ >= 0 ? slots_[active_].net->name() : nullptr;
  const char* to = target >= 0 ? slots_[target].net->name() : nullptr;
  VL_LOGI("networks: %s -> %s", from ? from : "none", to ? to : "none");
  if (engaged_) {
    transportOf(active_).disconnect();
    engaged_ = false;
    closedPending_ = true;
  }
  active_ = target;
  trying_ = false;
  if (target >= 0) slots_[target].net->select();
  if (changeCb_) changeCb_(from, to, changeCtx_);
}

Result NetworkSet::validate(const ConnectOptions& o) const {
  if (count_ == 0) {
    VL_LOGE("networks: no network added");
    return Result::Rejected;
  }
  for (size_t i = 0; i < count_; ++i) {
    if (!firstWith(i)) continue;
    Result r = slots_[i].net->transport().validate(o);
    if (r != Result::Ok) {
      VL_LOGE("networks: %s cannot make this connection", slots_[i].net->name());
      return r;
    }
  }
  return Result::Ok;
}

void NetworkSet::beginConnect(const ConnectOptions& o) {
  uint64_t now = clock_->uptimeMs();
  evaluate(now);
  if (active_ < 0) {
    VL_LOGW("networks: no network is up");
    closedPending_ = true;  // reported from loop(), never from here
    return;
  }
  if (!trying_) {
    trying_ = true;
    tryingSince_ = now;
  }
  lastAttempt_ = now;
  engaged_ = true;
  slots_[active_].net->select();  // the platform may have moved the default route since
  transportOf(active_).beginConnect(o);
}

void NetworkSet::disconnect() {
  if (active_ >= 0) transportOf(active_).disconnect();
  engaged_ = false;
  closedPending_ = false;
}

bool NetworkSet::connected() const { return active_ >= 0 && transportOf(active_).connected(); }

bool NetworkSet::publish(const char* topic, const uint8_t* p, size_t n, uint8_t qos, uint16_t* packetId) {
  return active_ >= 0 && transportOf(active_).publish(topic, p, n, qos, packetId);
}

void NetworkSet::subscribe(const char* filter, uint8_t qos) {
  if (active_ >= 0) transportOf(active_).subscribe(filter, qos);
}

void NetworkSet::loop() {
  for (size_t i = 0; i < count_; ++i) {
    if (firstWith(i)) slots_[i].net->transport().loop();
  }
  evaluate(clock_->uptimeMs());
  if (closedPending_) {
    closedPending_ = false;
    if (closedCb_) closedCb_(closedCtx_);
  }
}

size_t NetworkSet::maxPacket() const {
  size_t m = 0;
  for (size_t i = 0; i < count_; ++i) {
    size_t p = slots_[i].net->transport().maxPacket();
    if (i == 0 || p < m) m = p;
  }
  return m;
}

void NetworkSet::thunkConnack(uint8_t rc, void* ctx) {
  Slot* s = static_cast<Slot*>(ctx);
  NetworkSet* self = s->owner;
  if (!self->fromActive(s)) return;
  self->trying_ = false;  // any CONNACK proves the path
  if (self->connackCb_) self->connackCb_(rc, self->connackCtx_);
}

void NetworkSet::thunkSuback(const char* filter, bool granted, void* ctx) {
  Slot* s = static_cast<Slot*>(ctx);
  NetworkSet* self = s->owner;
  if (self->fromActive(s) && self->subackCb_) self->subackCb_(filter, granted, self->subackCtx_);
}

void NetworkSet::thunkPuback(uint16_t packetId, void* ctx) {
  Slot* s = static_cast<Slot*>(ctx);
  NetworkSet* self = s->owner;
  if (self->fromActive(s) && self->pubackCb_) self->pubackCb_(packetId, self->pubackCtx_);
}

void NetworkSet::thunkMessage(const char* topic, const uint8_t* p, size_t n, void* ctx) {
  Slot* s = static_cast<Slot*>(ctx);
  NetworkSet* self = s->owner;
  if (self->fromActive(s) && self->messageCb_) self->messageCb_(topic, p, n, self->messageCtx_);
}

void NetworkSet::thunkClosed(void* ctx) {
  Slot* s = static_cast<Slot*>(ctx);
  NetworkSet* self = s->owner;
  if (!self->fromActive(s)) return;
  self->engaged_ = false;
  if (self->closedCb_) self->closedCb_(self->closedCtx_);
}

bool NetworkSet::heapFree(uint32_t& bytes) { return platformSystemInfo().heapFree(bytes); }
bool NetworkSet::heapMin(uint32_t& bytes) { return platformSystemInfo().heapMin(bytes); }
const char* NetworkSet::resetReason() { return platformSystemInfo().resetReason(); }
const char* NetworkSet::iface() { return active_ >= 0 ? slots_[active_].net->name() : nullptr; }
bool NetworkSet::rssi(int16_t& dBm) { return active_ >= 0 && slots_[active_].net->rssi(dBm); }

}  // namespace vl
