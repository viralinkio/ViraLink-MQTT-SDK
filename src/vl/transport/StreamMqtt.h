#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vl/Config.h"
#include "vl/pal/Clock.h"
#include "vl/transport/IMqttTransport.h"

namespace vl {

// A byte stream to the broker: plain TCP, or TLS done by the stream itself.
// Used on the client task only.
class INetStream {
 public:
  virtual ~INetStream() {}
  // Whether the stream can connect as tls (Config::tls) describes. The default
  // is a plain stream, which refuses TLS rather than ignore it.
  virtual Result validate(const ITlsProvider* tls) const;
  // Blocks until connected or failed. tls is what validate() accepted.
  virtual bool connect(const char* host, uint16_t port, const ITlsProvider* tls) = 0;
  // Bytes read, 0 if none are waiting, -1 once the connection is gone.
  virtual int read(uint8_t* buf, size_t n) = 0;
  // Writes all n bytes, or returns false.
  virtual bool write(const uint8_t* p, size_t n) = 0;
  virtual void stop() = 0;
};

// IMqttTransport that speaks MQTT 3.1.1 itself over an INetStream: QoS 0/1
// with any number of publishes awaiting PUBACK, keep-alive pings, and inbound
// QoS 1 acknowledged at once. buf holds the largest inbound packet; larger
// ones are skipped (still acknowledged) and counted. connect() blocks for as
// long as the stream's connect does.
class StreamMqtt : public IMqttTransport {
 public:
  // clock null: the platform clock. buf must outlive the transport.
  StreamMqtt(INetStream& stream, uint8_t* buf, size_t cap, IClock* clock = nullptr);
  ~StreamMqtt() override;
  StreamMqtt(const StreamMqtt&) = delete;
  StreamMqtt& operator=(const StreamMqtt&) = delete;

  // The stream's verdict on Config::tls.
  Result validate(const ConnectOptions& o) const override;
  void beginConnect(const ConnectOptions& opts) override;
  void disconnect() override;
  bool connected() const override { return state_ == Online; }
  bool publish(const char* topic, const uint8_t* p, size_t n, uint8_t qos, uint16_t* packetId) override;
  void subscribe(const char* filter, uint8_t qos) override;
  void loop() override;
  size_t maxPacket() const override { return cap_; }

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

  // Inbound messages skipped: larger than the buffer, or a topic too long.
  uint32_t droppedMessages() const { return dropped_; }

 private:
  enum StateKind : uint8_t { Idle, AwaitConnack, Online };
  // Longer topics are skipped and counted. Lite subscribes to nothing longer
  // than v1/devices/me/attributes/response/<id>.
  static const size_t kTopicSize = VL_PROFILE_LITE ? 64 : 128;

  bool send(const uint8_t* p, size_t n);
  void close();
  bool receive();
  void dispatch(uint8_t type, uint8_t flags, const uint8_t* body, size_t n);
  uint16_t nextId();

  INetStream& stream_;
  uint8_t* buf_;
  size_t cap_;
  IClock* clock_;
  StateKind state_ = Idle;
  bool closedPending_ = false;  // report a failed connect from loop()
  size_t have_ = 0;             // bytes in buf_
  size_t skip_ = 0;             // bytes of an oversize packet still to discard
  uint16_t lastId_ = 0;
  uint32_t keepAliveMs_ = 0;
  uint64_t lastTx_ = 0;
  uint64_t lastRx_ = 0;
  uint32_t gen_ = 0;  // bumped whenever the connection (and buffer) is reset
  uint32_t dropped_ = 0;
  char topic_[kTopicSize];

  struct Sub {
    uint16_t id;
    const char* filter;  // the router's static string
  };
  Sub subs_[VL_MAX_SUBSCRIPTIONS] = {};

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

// StreamMqtt with its own buffer of N bytes.
template <size_t N = VL_TX_BUFFER_SIZE>
class BufferedStreamMqtt : public StreamMqtt {
 public:
  explicit BufferedStreamMqtt(INetStream& stream, IClock* clock = nullptr) : StreamMqtt(stream, buf_, N, clock) {}

 private:
  uint8_t buf_[N];
};

}  // namespace vl
