// ViraLink-MQTT-Client 1.0.2 compatibility: MQTTController and MQTTOTA on top
// of the v2 client, so a 1.0.2 sketch builds after replacing
// #include "viralink.h" with #include <ViraLinkCompat.h>. New code should use
// vl::Client directly; see docs/MIGRATING.md for what changed underneath.
//
// Like 1.0.2, this header works in Arduino Strings and ArduinoJson 7
// JsonDocuments, so the sketch, not the library, decides to use the heap:
// incoming messages are parsed into a JsonDocument for the 1.0.2 callbacks.
// Callbacks are plain function pointers: a lambda that captures nothing still
// converts, one that captures must become a function.
#pragma once

#if !defined(ARDUINO)
#error "ViraLinkCompat.h is for Arduino sketches written for ViraLink-MQTT-Client 1.0.2"
#endif

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Client.h>

#include <atomic>

#include "ViraLink.h"
#include "vl/compat/Bridge.h"

#if VL_PROFILE_LITE
#error "ViraLinkCompat.h needs the full profile; 1.0.2 never ran on AVR"
#endif

#if ARDUINOJSON_VERSION_MAJOR < 7
#error "ViraLinkCompat.h needs ArduinoJson 7, as ViraLink-MQTT-Client 1.0.2 did"
#endif

namespace vl {
namespace compat {

// INetStream over the Arduino Client the sketch last passed to connect().
// Used on the client task only.
class SwitchStream : public INetStream {
 public:
  void set(::Client* c) {
    if (client_ && client_ != c) client_->stop();
    client_ = c;
  }
  bool connect(const char* host, uint16_t port, const ITlsProvider* tls) override {
    (void)tls;
    return client_ && client_->connect(host, port) > 0;
  }
  int read(uint8_t* buf, size_t n) override {
    if (!client_) return -1;
    int waiting = client_->available();
    if (waiting <= 0) return client_->connected() ? 0 : -1;
    if (n > static_cast<size_t>(waiting)) n = static_cast<size_t>(waiting);
    int got = client_->read(buf, n);
    return got > 0 ? got : 0;
  }
  bool write(const uint8_t* p, size_t n) override {
    while (client_ && n) {
      size_t sent = client_->write(p, n);
      if (sent == 0 || sent > n) return false;
      p += sent;
      n -= sent;
    }
    return client_ != nullptr;
  }
  void stop() override {
    if (client_) client_->stop();
  }

 private:
  ::Client* client_ = nullptr;
};

}  // namespace compat
}  // namespace vl

class MQTTController {
 public:
  typedef bool (*DefaultMqttCallbackJsonPayload)(const String& topic, JsonDocument json);
  typedef bool (*DefaultMqttCallbackRawPayload)(const String& topic, uint8_t* payload, unsigned int length);
  typedef bool (*MqttCallbackJsonPayload)(String topic, JsonDocument json);
  typedef bool (*MqttCallbackRawPayload)(const String& topic, uint8_t* payload, unsigned int length);
  typedef void (*ConnectionEvent)();

  // queueSize is ignored: the outbox holds VL_COMPAT_OUTBOX_SIZE bytes, and
  // telemetry is dropped oldest first when it is full.
  explicit MQTTController(int queueSize = 512) : mqtt_(stream_), client_(mqtt_, outbox_) {
    (void)queueSize;
    bridge_.onMessage(onBridgeMessage, this);
#if VL_COMPAT_GATEWAY && VL_ENABLE_GATEWAY
    bridge_.setGateway(gw_);
    client_.use(bridge_, gw_);
#else
    client_.use(bridge_);
#endif
#if VL_ENABLE_HEALTH
    health_.setKeys(0);  // off until sendSystemAttributes(true), as in 1.0.2
    client_.use(health_);
#endif
    client_.onState(onState, this);
  }
  MQTTController(const MQTTController&) = delete;
  MQTTController& operator=(const MQTTController&) = delete;

  // Takes effect on the next loop(), on the task that runs it, and replaces
  // the previous connection: call it again after a network change. An empty
  // pass means an access token in username; otherwise MQTT basic
  // credentials. url, id, username and pass are copied.
  void connect(Client& client, String id, String username, String pass, String url, uint16_t port,
               DefaultMqttCallbackJsonPayload callback, DefaultMqttCallbackRawPayload callbackRaw = nullptr,
               ConnectionEvent connectionEvent = nullptr) {
    vl::LockGuard g(client_.lock());
    if (!copy(next_.host, sizeof(next_.host), url) || !copy(next_.id, sizeof(next_.id), id) ||
        !copy(next_.user, sizeof(next_.user), username) || !copy(next_.pass, sizeof(next_.pass), pass)) {
      VL_LOGE("compat: connect() argument too long, ignored");
      return;
    }
    next_.net = &client;
    next_.port = port;
    next_.json = callback;
    next_.raw = callbackRaw;
    next_.connected = connectionEvent;
    pending_ = true;
  }

  // Nothing to set up any more; kept so 1.0.2 sketches build.
  void init() {}

  // Run it from one task, as 1.0.2 did (on another core if you like).
  void loop() {
    if (pending_) apply();
#if VL_ENABLE_HEALTH
    bool on = healthOn_;
    uint32_t ms = healthMs_;
    if (on != healthApplied_ || ms != healthMsApplied_) {
      health_.setKeys(on ? vl::kHealthAll : 0);
      health_.setInterval(ms);
      healthApplied_ = on;
      healthMsApplied_ = ms;
    }
#endif
    if (started_) client_.loop();
  }

  bool isConnected() { return client_.online(); }

  // Accepted when the payload fits the transport buffer, which is fixed at
  // VL_TX_BUFFER_SIZE; nothing is reallocated.
  bool setBufferSize(uint16_t size) { return size <= VL_TX_BUFFER_SIZE; }
  void resetBufferSize() {}
  // The v2 session paces its own reconnects (backoff with jitter).
  void setTimeout(int timeoutMs) { (void)timeoutMs; }
  void resetTimeout() {}

  // payload is queued on topic if v2 knows the topic: device telemetry,
  // attributes, claims, attribute and RPC requests, RPC replies, and the
  // gateway connect, disconnect, attribute request and RPC topics. Gateway
  // data goes through sendTelemetry/sendAttributes with a device name.
  bool addToPublishQueue(const String& topic, const String& payload) {
    return vl::ok(bridge_.publish(topic.c_str(), payload.c_str(), payload.length()));
  }

  // keysJson: {"clientKeys":"a,b","sharedKeys":"c"}. The response goes to
  // callback, or without one to the registered and default callbacks. A
  // request that is never answered is dropped silently, as in 1.0.2.
  bool requestAttributesJson(const String& keysJson, MqttCallbackJsonPayload callback = nullptr) {
    if (!callback) {
      return vl::ok(bridge_.requestAttributes(keysJson.c_str(), keysJson.length(), onBridgeMessage, this));
    }
    return vl::ok(bridge_.requestAttributes(keysJson.c_str(), keysJson.length(), onResponse,
                                            reinterpret_cast<void*>(callback)));
  }

  // Device telemetry is stamped with the time it was queued; a sub-device
  // (deviceName, VL_COMPAT_GATEWAY) is connected first.
  bool sendTelemetry(const JsonDocument& json, const String& deviceName = "") {
    return send(json, deviceName, true);
  }
  bool sendTelemetry(const String& json) { return vl::ok(bridge_.telemetry(nullptr, json.c_str(), json.length())); }
  bool sendAttributes(const JsonDocument& json, const String& deviceName = "") {
    return send(json, deviceName, false);
  }
  bool sendAttributes(const String& json) { return vl::ok(bridge_.attributes(nullptr, json.c_str(), json.length())); }

  bool sendClaimRequest(const String& key, uint32_t durationMs, const String& deviceName = "") {
    return vl::ok(bridge_.claim(deviceName.c_str(), key.c_str(), durationMs));
  }

  // Gateway mode needs VL_COMPAT_GATEWAY 1 (and a gateway device); without
  // it these return false.
  bool sendGatewayConnectEvent(const String& deviceName) {
    return vl::ok(bridge_.connectDevice(deviceName.c_str()));
  }
  bool sendGatewayDisConnectEvent(const String& deviceName) {
    return vl::ok(bridge_.disconnectDevice(deviceName.c_str()));
  }
  // The gateway topics are subscribed on every connect; these decide whether
  // their messages reach the callbacks.
  bool subscribeToGatewayEvent() { return gatewayEvents(true); }
  bool unsubscribeToGatewayEvent() { return gatewayEvents(false); }

  // Callbacks tried in order before the default ones; at most
  // VL_COMPAT_CALLBACKS of each kind. Register them before connect().
  void registerCallbackRawPayload(MqttCallbackRawPayload callback) { add(raw_, rawCount_, callback); }
  void registerCallbackJsonPayload(MqttCallbackJsonPayload callback) { add(json_, jsonCount_, callback); }

  // Health attributes: vl_* keys, or the 1.0.2 names with VL_COMPAT_V1_KEYS.
  void sendSystemAttributes(bool value) { healthOn_ = value; }
  void updateSendSystemAttributesInterval(float seconds) {
    healthMs_ = seconds > 0 ? static_cast<uint32_t>(seconds * 1000) : 0;
  }

  // Records queued, sent or not.
  uint16_t getQueueSize() {
    size_t n = client_.stats().queuedRecords;
    return n > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(n);
  }

  // The v2 client underneath, for attaching more modules before connect().
  vl::Client& client() { return client_; }

 private:
  struct Next {
    ::Client* net = nullptr;
    char host[64] = {};
    char id[24] = {};
    char user[VL_USERNAME_SIZE] = {};
    char pass[VL_PASSWORD_SIZE] = {};
    uint16_t port = 1883;
    DefaultMqttCallbackJsonPayload json = nullptr;
    DefaultMqttCallbackRawPayload raw = nullptr;
    ConnectionEvent connected = nullptr;
  };

  static bool copy(char* dst, size_t cap, const String& s) {
    if (s.length() >= cap) return false;
    memcpy(dst, s.c_str(), s.length() + 1);
    return true;
  }

  template <class Cb>
  void add(Cb* table, size_t& count, Cb cb) {
    vl::LockGuard g(client_.lock());
    if (!cb || count >= VL_COMPAT_CALLBACKS) {
      VL_LOGE("compat: callback table full (VL_COMPAT_CALLBACKS)");
      return;
    }
    table[count] = cb;  // the entry before the count that publishes it
    ++count;
  }

  bool gatewayEvents(bool on) {
#if VL_COMPAT_GATEWAY && VL_ENABLE_GATEWAY
    gwEvents_ = on;
    return true;
#else
    (void)on;
    return false;
#endif
  }

  // On the client task: the connect() call replaces the session.
  void apply() {
    {
      vl::LockGuard g(client_.lock());
      cur_ = next_;
      pending_ = false;
    }
    client_.end();
    stream_.set(cur_.net);
    vl::Config cfg;
    cfg.host = cur_.host;
    cfg.port = cur_.port;
    cfg.clientId = cur_.id[0] ? cur_.id : nullptr;
    cfg.auth = cur_.pass[0] ? vl::MqttBasic(cfg.clientId, cur_.user, cur_.pass) : vl::AccessToken(cur_.user);
    vl::Result r = client_.begin(cfg);
    started_ = r == vl::Result::Ok;
    if (!started_) VL_LOGE("compat: connect to %s failed: %s", cur_.host, vl::toString(r));
  }

  bool send(const JsonDocument& doc, const String& device, bool telemetry) {
    vl::LockGuard g(bufLock_);
    size_t n = serializeJson(doc, buf_, sizeof(buf_));
    if (n == 0 || n >= sizeof(buf_) - 1) return false;
    const char* dev = device.c_str();
    return vl::ok(telemetry ? bridge_.telemetry(dev, buf_, n) : bridge_.attributes(dev, buf_, n));
  }

  static void onState(vl::State s, void* ctx) {
    MQTTController* self = static_cast<MQTTController*>(ctx);
    if (s == vl::State::Online && self->cur_.connected) self->cur_.connected();
  }

  static bool onResponse(const char* topic, const uint8_t* p, size_t n, void* ctx) {
    MqttCallbackJsonPayload cb = reinterpret_cast<MqttCallbackJsonPayload>(ctx);
    JsonDocument doc;
    if (deserializeJson(doc, p, n)) return false;
    return cb(String(topic), doc);
  }

  // Order as in 1.0.2: raw callbacks, then JSON callbacks, then the defaults.
  static bool onBridgeMessage(const char* topic, const uint8_t* p, size_t n, void* ctx) {
    MQTTController* self = static_cast<MQTTController*>(ctx);
#if VL_COMPAT_GATEWAY && VL_ENABLE_GATEWAY
    if (!self->gwEvents_ && strncmp(topic, "v1/gateway/", 11) == 0) return false;
#endif
    String t(topic);
    // 1.0.2 handed out a mutable buffer; v2's is the transport's, valid for this call.
    uint8_t* payload = const_cast<uint8_t*>(p);
    unsigned int len = static_cast<unsigned int>(n);
    for (size_t i = 0; i < self->rawCount_; ++i) {
      if (self->raw_[i](t, payload, len)) return true;
    }
    JsonDocument doc;
    if (deserializeJson(doc, p, n)) {
      if (self->cur_.raw) self->cur_.raw(t, payload, len);
      return true;
    }
    for (size_t i = 0; i < self->jsonCount_; ++i) {
      if (self->json_[i](t, doc)) return true;
    }
    if (self->cur_.json) self->cur_.json(t, doc);
    if (self->cur_.raw) self->cur_.raw(t, payload, len);
    return true;
  }

  vl::compat::SwitchStream stream_;
  vl::BufferedStreamMqtt<> mqtt_;
  vl::RingOutbox<VL_COMPAT_OUTBOX_SIZE> outbox_;
  vl::Client client_;
  vl::compat::Bridge bridge_;
#if VL_COMPAT_GATEWAY && VL_ENABLE_GATEWAY
  vl::Gateway<VL_COMPAT_GW_DEVICES> gw_;
  std::atomic<bool> gwEvents_{false};
#endif
#if VL_ENABLE_HEALTH
  vl::Health health_{10000};  // 1.0.2 sent system attributes every 10 s
  bool healthApplied_ = false;
  uint32_t healthMsApplied_ = 10000;
#endif
  std::atomic<bool> healthOn_{false};
  std::atomic<uint32_t> healthMs_{10000};

  Next next_;  // under client_.lock()
  Next cur_;   // client task only
  std::atomic<bool> pending_{false};
  bool started_ = false;

  MqttCallbackRawPayload raw_[VL_COMPAT_CALLBACKS] = {};
  size_t rawCount_ = 0;
  MqttCallbackJsonPayload json_[VL_COMPAT_CALLBACKS] = {};
  size_t jsonCount_ = 0;

  vl::PlatformLock bufLock_;
  char buf_[VL_TX_BUFFER_SIZE];
};

#if VL_ENABLE_OTA
// Firmware updates from the ThingsBoard OTA package assigned to the device,
// verified with any checksum algorithm ThingsBoard offers (1.0.2 took MD5
// only). The package is read on every connect and every fw_* update, so
// checkForUpdate() has nothing left to do.
class MQTTOTA {
 public:
  // chunkSize is ignored: chunks are sized from the transport buffer
  // (VL_OTA_CHUNK_SIZE caps them). Construct it before connect().
  MQTTOTA(MQTTController* controller, uint16_t chunkSize) : controller_(controller) {
    (void)chunkSize;
    if (controller_) controller_->client().attach(ota_);
  }
  MQTTOTA(const MQTTOTA&) = delete;
  MQTTOTA& operator=(const MQTTOTA&) = delete;

  bool begin(String currentFirmwareTitle, String currentFirmwareVersion) {
    return vl::ok(ota_.begin(currentFirmwareTitle.c_str(), currentFirmwareVersion.c_str()));
  }
  bool checkForUpdate() { return controller_ != nullptr; }
  // stop holds downloads (a download in progress starts over after start),
  // e.g. while on GSM. Reporting the running firmware goes on.
  void startHandleOTAMessages() { ota_.hold(false); }
  void stopHandleOTAMessages() { ota_.hold(true); }

  // The v2 module underneath, for onState()/onProgress().
  vl::Ota& ota() { return ota_; }

 private:
  MQTTController* controller_;
  vl::Ota ota_;
};
#endif  // VL_ENABLE_OTA
