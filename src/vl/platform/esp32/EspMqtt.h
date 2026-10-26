#pragma once

#include "vl/Config.h"

#if VL_PLATFORM_ESP32

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "vl/transport/IMqttTransport.h"

namespace vl {

// IMqttTransport over esp-mqtt, for ESP32 on both Arduino and ESP-IDF.
// esp-mqtt runs the socket on its own task; events are copied into a
// FreeRTOS ring buffer there and dispatched to the client's callbacks from
// loop(), so callbacks always run on the client task, in arrival order.
// Auto-reconnect is off: the client's Session decides when to reconnect.
//
// Buffers are allocated once, on the first beginConnect().
class EspMqtt : public IMqttTransport {
 public:
  // bufferSize: largest MQTT packet in either direction.
  // eventQueueBytes: room for events and received messages awaiting loop().
  explicit EspMqtt(size_t bufferSize = VL_TX_BUFFER_SIZE, size_t eventQueueBytes = 2 * VL_TX_BUFFER_SIZE);
  ~EspMqtt() override;

  // Refuses a secure element unless the build enables esp-tls support for
  // it, with the key slot and I2C address Kconfig sets.
  Result validate(const ConnectOptions& o) const override;
  void beginConnect(const ConnectOptions& opts) override;
  void disconnect() override;
  bool connected() const override { return connected_; }
  bool publish(const char* topic, const uint8_t* p, size_t n, uint8_t qos, uint16_t* packetId) override;
  void subscribe(const char* filter, uint8_t qos) override;
  void loop() override;
  size_t maxPacket() const override { return bufferSize_; }

  void onConnack(ConnackCb cb, void* ctx) override {
    connackCb_ = cb;
    connackCtx_ = ctx;
  }
  void onSuback(SubackCb cb, void* ctx) override {
    subackCb_ = cb;
    subackCtx_ = ctx;
  }
  void onPuback(PubackCb cb, void* ctx) override {
    pubackCb_ = cb;
    pubackCtx_ = ctx;
  }
  void onMessage(MessageCb cb, void* ctx) override {
    messageCb_ = cb;
    messageCtx_ = ctx;
  }
  void onClosed(ClosedCb cb, void* ctx) override {
    closedCb_ = cb;
    closedCtx_ = ctx;
  }

  // Messages dropped because the event queue was full or a message exceeded
  // bufferSize.
  uint32_t droppedMessages() const { return droppedMessages_; }

  // Internal: called from the esp-mqtt task.
  void handleEvent(int32_t eventId, void* eventData);

 private:
  enum : uint8_t { EvConnack, EvSuback, EvPuback, EvMessage, EvClosed };
  static const size_t kMaxSubs = 16;
  static const size_t kTopicMax = 128;

  bool allocate();
  void post(uint8_t type, uint8_t code, int msgId, const char* topic, size_t topicLen, const uint8_t* data,
            size_t dataLen);
  void flushQueue();

  size_t bufferSize_;
  size_t queueBytes_;
  void* client_ = nullptr;  // esp_mqtt_client_handle_t
  void* ring_ = nullptr;    // RingbufHandle_t
  uint8_t* stage_ = nullptr;
  char stageTopic_[kTopicMax] = {};
  size_t stageTopicLen_ = 0;
  size_t stageTotal_ = 0;
  bool stageDrop_ = false;
  std::atomic<bool> connected_{false};  // written by the esp-mqtt task
  std::atomic<uint32_t> droppedMessages_{0};

  struct Sub {
    int msgId;
    const char* filter;
  };
  Sub subs_[kMaxSubs] = {};

  ConnackCb connackCb_ = nullptr;
  void* connackCtx_ = nullptr;
  SubackCb subackCb_ = nullptr;
  void* subackCtx_ = nullptr;
  PubackCb pubackCb_ = nullptr;
  void* pubackCtx_ = nullptr;
  MessageCb messageCb_ = nullptr;
  void* messageCtx_ = nullptr;
  ClosedCb closedCb_ = nullptr;
  void* closedCtx_ = nullptr;
};

}  // namespace vl

#endif  // VL_PLATFORM_ESP32
