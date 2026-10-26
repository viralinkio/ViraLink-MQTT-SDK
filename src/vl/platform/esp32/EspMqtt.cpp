#include "vl/platform/esp32/EspMqtt.h"

#if VL_PLATFORM_ESP32

#include <stdlib.h>
#include <string.h>

#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "mqtt_client.h"
#include "vl/Log.h"

#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
// Declared here rather than via esp_crt_bundle.h: arduino-esp32 ships a
// different header of that name (WiFiClientSecure) that shadows IDF's.
extern "C" esp_err_t esp_crt_bundle_attach(void* conf);
#define VL_HAS_CRT_BUNDLE 1
#else
#define VL_HAS_CRT_BUNDLE 0
#endif

namespace vl {

namespace {

struct EventHdr {
  uint8_t type;
  uint8_t code;
  uint16_t topicLen;
  int32_t msgId;
  uint32_t dataLen;
};

void eventThunk(void* arg, esp_event_base_t, int32_t eventId, void* eventData) {
  static_cast<EspMqtt*>(arg)->handleEvent(eventId, eventData);
}

}  // namespace

EspMqtt::EspMqtt(size_t bufferSize, size_t eventQueueBytes) : bufferSize_(bufferSize), queueBytes_(eventQueueBytes) {}

EspMqtt::~EspMqtt() {
  disconnect();
  if (ring_) vRingbufferDelete(static_cast<RingbufHandle_t>(ring_));
  free(stage_);
}

bool EspMqtt::allocate() {
  if (ring_ && stage_) return true;
  if (!stage_) stage_ = static_cast<uint8_t*>(malloc(bufferSize_));
  if (!ring_) ring_ = xRingbufferCreate(queueBytes_, RINGBUF_TYPE_NOSPLIT);
  if (!stage_ || !ring_) {
    // All or nothing: the event path relies on both buffers existing.
    VL_LOGE("esp-mqtt: out of memory");
    if (ring_) vRingbufferDelete(static_cast<RingbufHandle_t>(ring_));
    free(stage_);
    ring_ = nullptr;
    stage_ = nullptr;
    return false;
  }
  return true;
}

void EspMqtt::beginConnect(const ConnectOptions& o) {
  disconnect();
  if (!allocate()) {
    post(EvClosed, 0, 0, nullptr, 0, nullptr, 0);
    return;
  }

  esp_mqtt_client_config_t c;
  memset(&c, 0, sizeof(c));
  const char* ca = o.tls ? o.tls->caPem() : nullptr;
  const char* cert = o.tls ? o.tls->clientCertPem() : nullptr;
  const char* key = o.tls ? o.tls->clientKeyPem() : nullptr;
#if ESP_IDF_VERSION_MAJOR >= 5
  c.broker.address.hostname = o.host;
  c.broker.address.port = o.port;
  c.broker.address.transport = o.tls ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP;
  c.broker.verification.certificate = ca;
#if VL_HAS_CRT_BUNDLE
  if (o.tls && !ca) c.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
#endif
  c.credentials.client_id = o.clientId;
  c.credentials.username = o.username;
  c.credentials.authentication.password = o.password;
  c.credentials.authentication.certificate = cert;
  c.credentials.authentication.key = key;
  c.session.keepalive = o.keepAliveS;
  c.session.disable_clean_session = false;
  c.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
  c.network.disable_auto_reconnect = true;
  c.buffer.size = static_cast<int>(bufferSize_);
  c.buffer.out_size = static_cast<int>(bufferSize_);
#else
  c.host = o.host;
  c.port = o.port;
  c.transport = o.tls ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP;
  c.cert_pem = ca;
#if VL_HAS_CRT_BUNDLE
  if (o.tls && !ca) c.crt_bundle_attach = esp_crt_bundle_attach;
#endif
  c.client_id = o.clientId;
  c.username = o.username;
  c.password = o.password;
  c.client_cert_pem = cert;
  c.client_key_pem = key;
  c.keepalive = o.keepAliveS;
  c.disable_clean_session = false;
  c.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
  c.disable_auto_reconnect = true;
  c.buffer_size = static_cast<int>(bufferSize_);
  c.out_buffer_size = static_cast<int>(bufferSize_);
#endif
  if (o.tls && !ca) {
#if !VL_HAS_CRT_BUNDLE
    VL_LOGE("esp-mqtt: TLS needs a CA (enable CONFIG_MBEDTLS_CERTIFICATE_BUNDLE or pass a CA PEM)");
    post(EvClosed, 0, 0, nullptr, 0, nullptr, 0);
    return;
#endif
  }

  esp_mqtt_client_handle_t h = esp_mqtt_client_init(&c);
  if (!h) {
    post(EvClosed, 0, 0, nullptr, 0, nullptr, 0);
    return;
  }
  esp_mqtt_client_register_event(h, static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID), eventThunk, this);
  client_ = h;
  if (esp_mqtt_client_start(h) != ESP_OK) {
    disconnect();
    post(EvClosed, 0, 0, nullptr, 0, nullptr, 0);
  }
}

void EspMqtt::disconnect() {
  connected_ = false;
  if (client_) {
    esp_mqtt_client_destroy(static_cast<esp_mqtt_client_handle_t>(client_));  // stops its task first
    client_ = nullptr;
  }
  flushQueue();
  memset(subs_, 0, sizeof(subs_));
}

void EspMqtt::flushQueue() {
  if (!ring_) return;
  RingbufHandle_t rb = static_cast<RingbufHandle_t>(ring_);
  size_t n;
  void* item;
  while ((item = xRingbufferReceive(rb, &n, 0)) != nullptr) vRingbufferReturnItem(rb, item);
}

bool EspMqtt::publish(const char* topic, const uint8_t* p, size_t n, uint8_t qos, uint16_t* packetId) {
  if (!client_ || !connected_) return false;
  int id = esp_mqtt_client_publish(static_cast<esp_mqtt_client_handle_t>(client_), topic,
                                   reinterpret_cast<const char*>(p), static_cast<int>(n), qos, 0);
  if (id < 0) return false;
  if (packetId) *packetId = static_cast<uint16_t>(id);
  return true;
}

void EspMqtt::subscribe(const char* filter, uint8_t qos) {
  if (!client_) return;
  int id = esp_mqtt_client_subscribe(static_cast<esp_mqtt_client_handle_t>(client_), filter, qos);
  if (id < 0) {
    if (subackCb_) subackCb_(filter, false, subackCtx_);
    return;
  }
  for (size_t i = 0; i < kMaxSubs; ++i) {
    if (!subs_[i].filter) {
      subs_[i].msgId = id;
      subs_[i].filter = filter;
      return;
    }
  }
  VL_LOGE("esp-mqtt: too many pending subscriptions");
}

void EspMqtt::post(uint8_t type, uint8_t code, int msgId, const char* topic, size_t topicLen, const uint8_t* data,
                   size_t dataLen) {
  if (!ring_) return;
  EventHdr h;
  h.type = type;
  h.code = code;
  h.msgId = msgId;
  h.topicLen = static_cast<uint16_t>(topicLen);
  h.dataLen = static_cast<uint32_t>(dataLen);
  size_t total = sizeof(h) + topicLen + 1 + dataLen;
  RingbufHandle_t rb = static_cast<RingbufHandle_t>(ring_);
  void* slot = nullptr;
  if (xRingbufferSendAcquire(rb, &slot, total, pdMS_TO_TICKS(type == EvMessage ? 0 : 100)) != pdTRUE) {
    if (type == EvMessage) ++droppedMessages_;
    VL_LOGW("esp-mqtt: event queue full, event %u dropped", type);
    return;
  }
  uint8_t* d = static_cast<uint8_t*>(slot);
  memcpy(d, &h, sizeof(h));
  if (topicLen) memcpy(d + sizeof(h), topic, topicLen);
  d[sizeof(h) + topicLen] = '\0';
  if (dataLen) memcpy(d + sizeof(h) + topicLen + 1, data, dataLen);
  xRingbufferSendComplete(rb, slot);
}

void EspMqtt::handleEvent(int32_t eventId, void* eventData) {
  esp_mqtt_event_handle_t e = static_cast<esp_mqtt_event_handle_t>(eventData);
  switch (static_cast<esp_mqtt_event_id_t>(eventId)) {
    case MQTT_EVENT_CONNECTED:
      connected_ = true;
      post(EvConnack, 0, 0, nullptr, 0, nullptr, 0);
      break;
    case MQTT_EVENT_DISCONNECTED:
      connected_ = false;
      post(EvClosed, 0, 0, nullptr, 0, nullptr, 0);
      break;
    case MQTT_EVENT_ERROR:
      if (e->error_handle && e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        post(EvConnack, static_cast<uint8_t>(e->error_handle->connect_return_code), 0, nullptr, 0, nullptr, 0);
      }
      break;
    case MQTT_EVENT_SUBSCRIBED: {
      uint8_t refused = 0;
#if ESP_IDF_VERSION_MAJOR >= 5
      if (e->data_len > 0 && static_cast<uint8_t>(e->data[0]) == 0x80) refused = 1;
#endif
      post(EvSuback, refused, e->msg_id, nullptr, 0, nullptr, 0);
      break;
    }
    case MQTT_EVENT_PUBLISHED:
      post(EvPuback, 0, e->msg_id, nullptr, 0, nullptr, 0);
      break;
    case MQTT_EVENT_DATA: {
      // Large messages arrive in fragments; the first one carries the topic.
      if (e->current_data_offset == 0) {
        stageTotal_ = static_cast<size_t>(e->total_data_len);
        stageDrop_ = stageTotal_ > bufferSize_ || static_cast<size_t>(e->topic_len) >= kTopicMax;
        stageTopicLen_ = stageDrop_ ? 0 : static_cast<size_t>(e->topic_len);
        if (stageTopicLen_) memcpy(stageTopic_, e->topic, stageTopicLen_);
        if (stageDrop_) {
          ++droppedMessages_;
          VL_LOGW("esp-mqtt: incoming message of %u bytes exceeds the buffer", static_cast<unsigned>(stageTotal_));
        }
      }
      if (stageDrop_ || !stage_) break;
      size_t off = static_cast<size_t>(e->current_data_offset);
      size_t n = static_cast<size_t>(e->data_len);
      if (off + n > bufferSize_) break;
      memcpy(stage_ + off, e->data, n);
      if (off + n == stageTotal_) post(EvMessage, 0, 0, stageTopic_, stageTopicLen_, stage_, stageTotal_);
      break;
    }
    default:
      break;
  }
}

void EspMqtt::loop() {
  if (!ring_) return;
  RingbufHandle_t rb = static_cast<RingbufHandle_t>(ring_);
  // Bounded so a flood of messages cannot starve the rest of the client loop.
  for (int i = 0; i < 16; ++i) {
    size_t n = 0;
    uint8_t* d = static_cast<uint8_t*>(xRingbufferReceive(rb, &n, 0));
    if (!d) return;
    EventHdr h;
    memcpy(&h, d, sizeof(h));
    const char* topic = reinterpret_cast<const char*>(d + sizeof(h));
    const uint8_t* data = d + sizeof(h) + h.topicLen + 1;
    switch (h.type) {
      case EvConnack:
        if (connackCb_) connackCb_(h.code, connackCtx_);
        break;
      case EvClosed:
        if (closedCb_) closedCb_(closedCtx_);
        break;
      case EvPuback:
        if (pubackCb_) pubackCb_(static_cast<uint16_t>(h.msgId), pubackCtx_);
        break;
      case EvSuback:
        for (size_t s = 0; s < kMaxSubs; ++s) {
          if (subs_[s].filter && subs_[s].msgId == h.msgId) {
            const char* f = subs_[s].filter;
            subs_[s].filter = nullptr;
            if (subackCb_) subackCb_(f, h.code == 0, subackCtx_);
            break;
          }
        }
        break;
      case EvMessage:
        if (messageCb_) messageCb_(topic, data, h.dataLen, messageCtx_);
        break;
    }
    vRingbufferReturnItem(rb, d);
  }
}

}  // namespace vl

#endif  // VL_PLATFORM_ESP32
