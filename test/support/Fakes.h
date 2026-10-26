// Test doubles: a manual clock and a scripted MQTT transport.
#pragma once

#include <string>
#include <vector>

#include "vl/pal/Clock.h"
#include "vl/transport/IMqttTransport.h"

namespace vltest {

class FakeClock : public vl::IClock {
 public:
  uint64_t now = 1000;
  bool haveUtc = false;
  uint64_t utcAtZero = 0;  // utc = utcAtZero + now

  uint64_t uptimeMs() override { return now; }
  bool utcMs(uint64_t& out) override {
    if (!haveUtc) return false;
    out = utcAtZero + now;
    return true;
  }
  void advance(uint64_t ms) { now += ms; }
};

struct Published {
  std::string topic;
  std::string payload;
  uint16_t packetId;
};

class FakeTransport : public vl::IMqttTransport {
 public:
  // Script
  int connectCalls = 0;
  int disconnectCalls = 0;
  bool isConnected = false;
  bool failPublish = false;
  size_t packet = 1024;
  uint16_t nextPacketId = 1;
  vl::ConnectOptions lastOpts;
  std::string lastUser;
  std::vector<std::string> subscribed;
  std::vector<Published> published;

  // IMqttTransport
  void beginConnect(const vl::ConnectOptions& o) override {
    ++connectCalls;
    lastOpts = o;
    lastUser = o.username ? o.username : "";
    isConnected = false;
  }
  void disconnect() override {
    ++disconnectCalls;
    isConnected = false;
  }
  bool connected() const override { return isConnected; }
  bool publish(const char* topic, const uint8_t* p, size_t n, uint8_t qos, uint16_t* packetId) override {
    (void)qos;
    if (failPublish || !isConnected) return false;
    Published m;
    m.topic = topic;
    m.payload.assign(reinterpret_cast<const char*>(p), n);
    m.packetId = nextPacketId++;
    if (packetId) *packetId = m.packetId;
    published.push_back(m);
    return true;
  }
  void subscribe(const char* filter, uint8_t qos) override {
    (void)qos;
    subscribed.push_back(filter);
  }
  void loop() override {}
  size_t maxPacket() const override { return packet; }

  void onConnack(ConnackCb cb, void* ctx) override {
    connackCb = cb;
    connackCtx = ctx;
  }
  void onSuback(SubackCb cb, void* ctx) override {
    subackCb = cb;
    subackCtx = ctx;
  }
  void onPuback(PubackCb cb, void* ctx) override {
    pubackCb = cb;
    pubackCtx = ctx;
  }
  void onMessage(MessageCb cb, void* ctx) override {
    messageCb = cb;
    messageCtx = ctx;
  }
  void onClosed(ClosedCb cb, void* ctx) override {
    closedCb = cb;
    closedCtx = ctx;
  }

  // Broker side
  void connack(uint8_t rc) {
    isConnected = rc == 0;
    if (connackCb) connackCb(rc, connackCtx);
  }
  void subackAll(bool granted = true) {
    std::vector<std::string> subs = subscribed;
    subscribed.clear();
    for (size_t i = 0; i < subs.size(); ++i) {
      if (subackCb) subackCb(subs[i].c_str(), granted, subackCtx);
    }
  }
  void puback(uint16_t id) {
    if (pubackCb) pubackCb(id, pubackCtx);
  }
  void pubackAll() {
    std::vector<Published> p = published;
    for (size_t i = 0; i < p.size(); ++i) puback(p[i].packetId);
  }
  void message(const char* topic, const std::string& payload) {
    if (messageCb) messageCb(topic, reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), messageCtx);
  }
  void close() {
    isConnected = false;
    if (closedCb) closedCb(closedCtx);
  }

  ConnackCb connackCb = nullptr;
  void* connackCtx = nullptr;
  SubackCb subackCb = nullptr;
  void* subackCtx = nullptr;
  PubackCb pubackCb = nullptr;
  void* pubackCtx = nullptr;
  MessageCb messageCb = nullptr;
  void* messageCtx = nullptr;
  ClosedCb closedCb = nullptr;
  void* closedCtx = nullptr;
};

}  // namespace vltest
