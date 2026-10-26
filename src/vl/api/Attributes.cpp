#include "vl/api/Attributes.h"

#include <float.h>
#include <string.h>

#include "vl/Client.h"
#include "vl/Log.h"
#include "vl/core/Codec.h"

namespace vl {

namespace {

// True if key is one of the names in the comma-separated list.
bool listHas(const char* list, const char* key) {
  size_t n = strlen(key);
  for (const char* s = list;;) {
    const char* comma = strchr(s, ',');
    size_t len = comma ? static_cast<size_t>(comma - s) : strlen(s);
    if (len == n && memcmp(s, key, n) == 0) return true;
    if (!comma) return false;
    s = comma + 1;
  }
}

const char* orNull(const char* s) { return s && s[0] ? s : nullptr; }

}  // namespace

Result SharedAttributesBase::add(const char* keys, Kind kind, void* var, size_t cap, AttrUpdateCb cb,
                                 AttrChangedCb changed, void* ctx) {
  size_t n = keys ? strlen(keys) : 0;
  if (n >= VL_ATTR_KEYS_SIZE) return Result::Rejected;
  if (count_ >= cap_) return Result::NoMemory;
  Entry& e = table_[count_++];
  memcpy(e.keys, keys ? keys : "", n + 1);
  e.kind = kind;
  e.var = var;
  e.cap = cap;
  e.cb = cb;
  e.changed = changed;
  e.ctx = ctx;
  return Result::Ok;
}

Result SharedAttributesBase::onUpdate(const char* keys, AttrUpdateCb cb, void* ctx) {
  if (!cb) return Result::Rejected;
  return add(keys, Kind::Handler, nullptr, 0, cb, nullptr, ctx);
}

Result SharedAttributesBase::bindVar(const char* key, Kind kind, void* var, size_t cap, AttrChangedCb changed,
                                     void* ctx) {
  // A comma would split the key when the bound keys are requested.
  if (!key || !key[0] || strchr(key, ',') || !var) return Result::Rejected;
  return add(key, kind, var, cap, nullptr, changed, ctx);
}

Result SharedAttributesBase::bind(const char* key, bool* var, AttrChangedCb changed, void* ctx) {
  return bindVar(key, Kind::Bool, var, 0, changed, ctx);
}

Result SharedAttributesBase::bind(const char* key, int32_t* var, AttrChangedCb changed, void* ctx) {
  return bindVar(key, Kind::Int32, var, 0, changed, ctx);
}

Result SharedAttributesBase::bind(const char* key, float* var, AttrChangedCb changed, void* ctx) {
  return bindVar(key, Kind::Float, var, 0, changed, ctx);
}

Result SharedAttributesBase::bind(const char* key, char* buf, size_t cap, AttrChangedCb changed, void* ctx) {
  if (cap == 0) return Result::Rejected;
  return bindVar(key, Kind::Text, buf, cap, changed, ctx);
}

Result SharedAttributesBase::request(const char* clientKeys, const char* sharedKeys, AttrResponseCb cb, void* ctx,
                                     uint32_t timeoutMs) {
  clientKeys = orNull(clientKeys);
  sharedKeys = orNull(sharedKeys);
  if (!clientKeys && !sharedKeys) return Result::Rejected;
  if (!client_) return Result::NotReady;
  char buf[VL_ATTR_KEYS_SIZE * 4];
  JsonWriter w(buf, sizeof(buf));
  codec::attributeRequest(w, clientKeys, sharedKeys);
  if (w.overflowed()) return Result::TooLarge;
  RecordMeta m;
  m.topic = OutTopic::AttrRequest;
  m.cls = RecordClass::Control;
  return requests_.start(*client_, this, m, buf, w.size(), timeoutMs, onResponse, cb, ctx);
}

void SharedAttributesBase::topics(TopicSink& sink) {
  sink.add(InTopic::AttrUpdate);
  sink.add(InTopic::AttrResponse);
}

bool SharedAttributesBase::handle(const Topic& topic, Bytes payload) {
  if (topic.kind == InTopic::AttrResponse) return client_->resolvePending(topic.id, payload.data(), payload.size());
  if (topic.kind != InTopic::AttrUpdate) return false;
  if (!forEachAttribute(JsonValue::parse(payload), visitUpdate, this)) VL_LOGW("attributes: update is not a JSON object");
  return true;
}

bool forEachAttribute(JsonValue attrs, AttrVisitor fn, void* ctx) {
  if (attrs.type() != JsonValue::Type::Object) return false;
  // Keys that do not fit VL_ATTR_KEYS_SIZE cannot match a handler and are skipped.
  char key[VL_ATTR_KEYS_SIZE];
  JsonValue k, v;
  for (size_t i = 0; attrs.member(i, k, v); ++i) {
    if (!k.copyString(key, sizeof(key))) continue;
    if (strcmp(key, "deleted") == 0 && v.type() == JsonValue::Type::Array) {
      for (size_t j = 0; j < v.size(); ++j) {
        if (v.at(j).copyString(key, sizeof(key))) fn(key, JsonValue(), ctx);
      }
      continue;
    }
    fn(key, v, ctx);
  }
  return true;
}

void SharedAttributesBase::dispatch(const char* key, JsonValue value) {
  for (size_t i = 0; i < count_; ++i) {
    Entry& e = table_[i];
    if (e.kind != Kind::Handler || (e.keys[0] && !listHas(e.keys, key))) continue;
    e.cb(key, value, e.ctx);
  }
  applyBindings(key, value);
}

void SharedAttributesBase::applyBindings(const char* key, JsonValue value) {
  for (size_t i = 0; i < count_; ++i) {
    Entry& e = table_[i];
    if (e.kind == Kind::Handler || strcmp(e.keys, key) != 0) continue;
    if (assign(e, value) && e.changed) e.changed(key, e.ctx);
  }
}

void SharedAttributesBase::onOnline() {
  // Bound variables must be right after a reboot, not only after the next
  // update: fetch their current values.
  char keys[VL_ATTR_KEYS_SIZE * 2];
  size_t n = 0;
  for (size_t i = 0; i < count_; ++i) {
    const Entry& e = table_[i];
    if (e.kind == Kind::Handler) continue;
    size_t len = strlen(e.keys);
    if (n + len + 2 > sizeof(keys)) {
      VL_LOGW("attributes: binding %s not refreshed, raise VL_ATTR_KEYS_SIZE", e.keys);
      continue;
    }
    if (n) keys[n++] = ',';
    memcpy(keys + n, e.keys, len + 1);
    n += len;
  }
  if (n == 0) return;
  Result r = request(nullptr, keys, onRefresh, this);
  if (!ok(r)) VL_LOGW("attributes: binding refresh not sent: %s", toString(r));
}

void SharedAttributesBase::onRefresh(Result r, JsonValue client, JsonValue shared, void* ctx) {
  (void)client;
  if (r != Result::Ok) {
    VL_LOGW("attributes: binding refresh failed: %s", toString(r));
    return;
  }
  SharedAttributesBase* self = static_cast<SharedAttributesBase*>(ctx);
  char key[VL_ATTR_KEYS_SIZE];
  JsonValue k, v;
  for (size_t i = 0; shared.member(i, k, v); ++i) {
    if (k.copyString(key, sizeof(key))) self->applyBindings(key, v);
  }
}

bool SharedAttributesBase::assign(const Entry& e, JsonValue value) {
  bool deleted = !value.valid();
  switch (e.kind) {
    case Kind::Bool: {
      if (!deleted && value.type() != JsonValue::Type::Bool) return false;
      bool v = !deleted && value.asBool();
      bool* var = static_cast<bool*>(e.var);
      if (*var == v) return false;
      *var = v;
      return true;
    }
    case Kind::Int32: {
      int32_t v = 0;
      if (!deleted) {
        // The coarse double check keeps asInt() in range; the exact one is
        // needed where double is 32-bit.
        double d = value.asDouble(0);
        if (value.type() != JsonValue::Type::Number || d < -2147483649.0 || d > 2147483648.0) return false;
        long long x = value.asInt();
        if (x < -2147483647LL - 1 || x > 2147483647LL) return false;
        v = static_cast<int32_t>(x);
      }
      int32_t* var = static_cast<int32_t*>(e.var);
      if (*var == v) return false;
      *var = v;
      return true;
    }
    case Kind::Float: {
      float v = 0;
      if (!deleted) {
        double d = value.asDouble(0);
        if (value.type() != JsonValue::Type::Number || d < -FLT_MAX || d > FLT_MAX) return false;
        v = static_cast<float>(d);
      }
      float* var = static_cast<float*>(e.var);
      if (*var == v) return false;
      *var = v;
      return true;
    }
    case Kind::Text: {
      char* buf = static_cast<char*>(e.var);
      if (deleted) {
        if (!buf[0]) return false;
        buf[0] = '\0';
        return true;
      }
      if (value.type() != JsonValue::Type::String || value.equals(buf)) return false;
      return value.copyString(buf, e.cap);  // leaves buf untouched if too long
    }
    case Kind::Handler:
      break;
  }
  return false;
}

void SharedAttributesBase::onResponse(Result r, const uint8_t* p, size_t n, void* ctx) {
  AttrResponseCb cb;
  void* userCtx;
  RequestSlots<AttrResponseCb, VL_ATTR_MAX_REQUESTS>::finish(ctx, cb, userCtx);
  if (!cb) return;
  JsonValue client, shared;
  if (r == Result::Ok) {
    JsonValue v = JsonValue::parse(reinterpret_cast<const char*>(p), n);
    if (v.type() == JsonValue::Type::Object) {
      client = v["client"];
      shared = v["shared"];
    } else {
      r = Result::Rejected;
    }
  }
  cb(r, client, shared, userCtx);
}

Result ClientAttributesBase::add(const char* key, Kind kind, const void* var, uint32_t minIntervalMs) {
  if (!key || !key[0] || !var) return Result::Rejected;
  size_t n = strlen(key);
  if (n >= VL_ATTR_KEYS_SIZE) return Result::Rejected;
  if (count_ >= cap_) return Result::NoMemory;
  Entry& e = table_[count_++];
  memcpy(e.key, key, n + 1);
  e.kind = kind;
  e.reported = false;
  e.var = var;
  e.minIntervalMs = minIntervalMs;
  e.lastMs = 0;
  memset(&e.last, 0, sizeof(e.last));
  return Result::Ok;
}

Result ClientAttributesBase::report(const char* key, const bool* var, uint32_t minIntervalMs) {
  return add(key, Kind::Bool, var, minIntervalMs);
}

Result ClientAttributesBase::report(const char* key, const int32_t* var, uint32_t minIntervalMs) {
  return add(key, Kind::Int32, var, minIntervalMs);
}

Result ClientAttributesBase::report(const char* key, const float* var, uint32_t minIntervalMs) {
  return add(key, Kind::Float, var, minIntervalMs);
}

bool ClientAttributesBase::handle(const Topic& topic, Bytes payload) {
  (void)topic;
  (void)payload;
  return false;
}

void ClientAttributesBase::tick(uint64_t nowMs) {
  if (!client_) return;
  for (size_t i = 0; i < count_; ++i) {
    Entry& e = table_[i];
    Value cur;
    memset(&cur, 0, sizeof(cur));
    switch (e.kind) {
      case Kind::Bool:
        cur.b = *static_cast<const bool*>(e.var);
        break;
      case Kind::Int32:
        cur.i = *static_cast<const int32_t*>(e.var);
        break;
      case Kind::Float:
        cur.f = *static_cast<const float*>(e.var);
        break;
    }
    // Bitwise, so a NaN that stays NaN is not reported again and again.
    bool changed = memcmp(&cur, &e.last, sizeof(cur)) != 0;
    if (e.reported && (!changed || nowMs - e.lastMs < e.minIntervalMs)) continue;
    PayloadBuilder<VL_ATTR_KEYS_SIZE + 32> b = client_->attributes<VL_ATTR_KEYS_SIZE + 32>();
    switch (e.kind) {
      case Kind::Bool:
        b.add(e.key, cur.b);
        break;
      case Kind::Int32:
        b.add(e.key, cur.i);
        break;
      case Kind::Float:
        b.add(e.key, cur.f);
        break;
    }
    if (!ok(b.send())) continue;  // retried on the next tick
    e.last = cur;
    e.lastMs = nowMs;
    e.reported = true;
  }
}

}  // namespace vl
