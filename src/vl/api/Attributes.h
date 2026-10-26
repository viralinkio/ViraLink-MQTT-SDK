#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vl/Config.h"
#include "vl/IApiModule.h"
#include "vl/Result.h"
#include "vl/api/Requests.h"
#include "vl/core/JsonReader.h"

namespace vl {

// value is Invalid when the attribute was deleted.
typedef void (*AttrUpdateCb)(const char* key, JsonValue value, void* ctx);
// client and shared are the response sections; Invalid when absent or when r
// is not Ok (Timeout, NotReady after end(), Rejected for a malformed reply).
typedef void (*AttrResponseCb)(Result r, JsonValue client, JsonValue shared, void* ctx);
// A bound variable was rewritten with a different value.
typedef void (*AttrChangedCb)(const char* key, void* ctx);

typedef void (*AttrVisitor)(const char* key, JsonValue value, void* ctx);
// Calls fn for every member of an attribute object and, with an Invalid
// value, for every name in its "deleted" array. Keys longer than
// VL_ATTR_KEYS_SIZE - 1 are skipped. False if attrs is not an object.
bool forEachAttribute(JsonValue attrs, AttrVisitor fn, void* ctx);

// Shared attributes (server to device): update handlers per key list,
// variables bound to keys, and attribute requests. Key lists are
// comma-separated without spaces, e.g. "mode,interval". Handlers and
// bindings share the capacity N of SharedAttributes<N>.
class SharedAttributesBase : public ApiModule {
 public:
  // Fires cb for every updated or deleted key in keys (null = every key).
  // Call before begin().
  Result onUpdate(const char* keys, AttrUpdateCb cb, void* ctx = nullptr);
  // Keeps *var equal to shared attribute key: requested on every Online,
  // rewritten on each update, zeroed (false, 0, "") when deleted. A value of
  // another type, out of range or too long is ignored. Variables are written
  // on the client task only. Call before begin().
  Result bind(const char* key, bool* var, AttrChangedCb changed = nullptr, void* ctx = nullptr);
  Result bind(const char* key, int32_t* var, AttrChangedCb changed = nullptr, void* ctx = nullptr);
  Result bind(const char* key, float* var, AttrChangedCb changed = nullptr, void* ctx = nullptr);
  Result bind(const char* key, char* buf, size_t cap, AttrChangedCb changed = nullptr, void* ctx = nullptr);
  // Requests client and/or shared values (null = none; not both). cb fires
  // once with the response or Timeout. Thread-safe.
  Result request(const char* clientKeys, const char* sharedKeys, AttrResponseCb cb, void* ctx,
                 uint32_t timeoutMs = VL_ATTR_TIMEOUT_MS);

  void topics(TopicSink& sink) override;
  bool handle(const Topic& topic, Bytes payload) override;
  void onOnline() override;

 protected:
  enum class Kind : uint8_t { Handler, Bool, Int32, Float, Text };
  struct Entry {
    char keys[VL_ATTR_KEYS_SIZE];  // handler: key list, "" = every key; binding: its key
    Kind kind;
    void* var;              // bindings
    size_t cap;             // Text
    AttrUpdateCb cb;        // Handler
    AttrChangedCb changed;  // bindings, may be null
    void* ctx;
  };
  SharedAttributesBase(Entry* table, size_t cap) : table_(table), cap_(cap) {}

 private:
  Result add(const char* keys, Kind kind, void* var, size_t cap, AttrUpdateCb cb, AttrChangedCb changed, void* ctx);
  Result bindVar(const char* key, Kind kind, void* var, size_t cap, AttrChangedCb changed, void* ctx);
  void dispatch(const char* key, JsonValue value);
  static void visitUpdate(const char* key, JsonValue v, void* ctx) {
    static_cast<SharedAttributesBase*>(ctx)->dispatch(key, v);
  }
  void applyBindings(const char* key, JsonValue value);
  // Writes value (Invalid = deleted) into a binding; true if it changed.
  static bool assign(const Entry& e, JsonValue value);
  static void onResponse(Result r, const uint8_t* p, size_t n, void* ctx);
  static void onRefresh(Result r, JsonValue client, JsonValue shared, void* ctx);

  Entry* table_;
  size_t cap_;
  size_t count_ = 0;
  RequestSlots<AttrResponseCb, VL_ATTR_MAX_REQUESTS> requests_;
};

// Shared-attribute module with room for N update handlers and bindings.
template <size_t N>
class SharedAttributes : public SharedAttributesBase {
 public:
  SharedAttributes() : SharedAttributesBase(entries_, N) {}

 private:
  Entry entries_[N];
};

// Client attributes (device to server) reported from variables: published on
// the first tick, then whenever the value changed and at least minIntervalMs
// passed since its last report. The variables are read on the client task.
// Use ClientAttributes<N>.
class ClientAttributesBase : public ApiModule {
 public:
  // The key is copied. Call before begin().
  Result report(const char* key, const bool* var, uint32_t minIntervalMs = 0);
  Result report(const char* key, const int32_t* var, uint32_t minIntervalMs = 0);
  Result report(const char* key, const float* var, uint32_t minIntervalMs = 0);

  void topics(TopicSink& sink) override { (void)sink; }
  bool handle(const Topic& topic, Bytes payload) override;
  void tick(uint64_t nowMs) override;

 protected:
  enum class Kind : uint8_t { Bool, Int32, Float };
  union Value {
    bool b;
    int32_t i;
    float f;
  };
  struct Entry {
    char key[VL_ATTR_KEYS_SIZE];
    Kind kind;
    bool reported;
    const void* var;
    uint32_t minIntervalMs;
    uint64_t lastMs;
    Value last;
  };
  ClientAttributesBase(Entry* table, size_t cap) : table_(table), cap_(cap) {}

 private:
  Result add(const char* key, Kind kind, const void* var, uint32_t minIntervalMs);

  Entry* table_;
  size_t cap_;
  size_t count_ = 0;
};

// Client-attribute module with room for N reported variables.
template <size_t N>
class ClientAttributes : public ClientAttributesBase {
 public:
  ClientAttributes() : ClientAttributesBase(entries_, N) {}

 private:
  Entry entries_[N];
};

}  // namespace vl
