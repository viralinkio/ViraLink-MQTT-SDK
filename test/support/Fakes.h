// Test doubles: a manual clock and a scripted MQTT transport.
#pragma once

#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "vl/pal/Clock.h"
#include "vl/pal/KeyValueStore.h"
#include "vl/pal/OutboxStore.h"
#include "vl/pal/SystemInfo.h"
#include "vl/transport/IMqttTransport.h"

namespace vltest {

class FakeClock : public vl::IClock {
 public:
  uint64_t now = 1000;
  bool haveUtc = false;
  uint64_t utcAtZero = 0;  // utc = utcAtZero + now
  uint64_t step = 0;       // added after every uptimeMs(), for code that polls

  uint64_t uptimeMs() override {
    uint64_t t = now;
    now += step;
    return t;
  }
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
  bool link = true;  // what linkUp() answers
  vl::Result verdict = vl::Result::Ok;   // what validate() answers
  mutable vl::ConnectOptions validated;  // what it was asked
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
  bool linkUp() const override { return link; }
  vl::Result validate(const vl::ConnectOptions& o) const override {
    validated = o;
    return verdict;
  }
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

class MemoryStore : public vl::IKeyValueStore {
 public:
  std::map<std::string, std::string> data;
  bool failSet = false;

  vl::Result get(const char* key, void* buf, size_t cap, size_t& len) override {
    std::map<std::string, std::string>::const_iterator it = data.find(key);
    if (it == data.end()) return vl::Result::Rejected;
    if (it->second.size() > cap) return vl::Result::TooLarge;
    memcpy(buf, it->second.data(), it->second.size());
    len = it->second.size();
    return vl::Result::Ok;
  }
  vl::Result set(const char* key, const void* value, size_t len) override {
    if (failSet) return vl::Result::Rejected;
    data[key].assign(static_cast<const char*>(value), len);
    return vl::Result::Ok;
  }
  vl::Result erase(const char* key) override {
    data.erase(key);
    return vl::Result::Ok;
  }
};

// Scripted chip vitals; a value is unknown while its has* flag is false.
class FakeSystemInfo : public vl::ISystemInfo {
 public:
  bool hasHeap = true;
  uint32_t free = 180000;
  uint32_t min = 150000;
  const char* reason = "power_on";
  const char* link = "wifi";
  bool hasRssi = true;
  int16_t dBm = -61;

  bool heapFree(uint32_t& bytes) override {
    bytes = free;
    return hasHeap;
  }
  bool heapMin(uint32_t& bytes) override {
    bytes = min;
    return hasHeap;
  }
  const char* resetReason() override { return reason; }
  const char* iface() override { return link; }
  bool rssi(int16_t& out) override {
    out = dBm;
    return hasRssi;
  }
};

// NOR flash: erase sets a segment to 0xFF, a write can only clear bits. A
// write budget models a power cut: the write that exhausts it stops part way
// with one half-programmed byte; an erase costs 2, so the cut falls either
// before it or half way through it; every later call fails until
// powerCycle(). With loseUnsynced (a file system) the cut also undoes every
// write not yet synced.
class FakeFlash : public vl::IOutboxStore {
 public:
  size_t count;
  size_t size;
  std::vector<uint8_t> mem;
  long budget = -1;          // bytes that may still be written; an erase costs 1; -1 = no limit
  bool dead = false;         // power is off
  bool failNext = false;     // the next call returns an error
  bool overwrote = false;    // a write hit bytes that were not erased
  bool loseUnsynced = false;
  int writes = 0;
  int erases = 0;
  int syncs = 0;

  FakeFlash(size_t segments, size_t segSize)
      : count(segments), size(segSize), mem(segments * segSize, 0xFF), unsyncedFrom(segments, segSize) {}

  void powerCycle() {
    dead = false;
    budget = -1;
  }
  size_t segmentCount() const override { return count; }
  size_t segmentSize() const override { return size; }

  vl::Result read(size_t seg, size_t off, uint8_t* buf, size_t n, size_t& got) override {
    got = 0;
    if (refuse() || seg >= count || off > size) return vl::Result::Rejected;
    got = n < size - off ? n : size - off;
    memcpy(buf, &mem[seg * size + off], got);
    return vl::Result::Ok;
  }
  vl::Result write(size_t seg, size_t off, const uint8_t* p, size_t n) override {
    if (refuse() || seg >= count || off + n > size) return vl::Result::Rejected;
    ++writes;
    size_t ok = n;
    bool cut = budget >= 0 && static_cast<long>(n) > budget;
    if (cut) ok = static_cast<size_t>(budget);
    uint8_t* d = &mem[seg * size + off];
    for (size_t i = 0; i < ok; ++i) {
      if (d[i] != 0xFF) overwrote = true;
      d[i] &= p[i];
    }
    if (off < unsyncedFrom[seg]) unsyncedFrom[seg] = off;
    if (cut) {
      d[ok] &= static_cast<uint8_t>(p[ok] | 0x5A);  // half-programmed
      powerFail();
      return vl::Result::Rejected;
    }
    if (budget >= 0) budget -= static_cast<long>(n);
    return vl::Result::Ok;
  }
  vl::Result erase(size_t seg) override {
    if (refuse() || seg >= count) return vl::Result::Rejected;
    if (budget == 0 || budget == 1) {
      if (budget == 1) {
        ++erases;
        memset(&mem[seg * size], 0xFF, size / 2);
        unsyncedFrom[seg] = size;
      }
      powerFail();
      return vl::Result::Rejected;
    }
    ++erases;
    if (budget > 0) budget -= 2;
    memset(&mem[seg * size], 0xFF, size);
    unsyncedFrom[seg] = size;
    return vl::Result::Ok;
  }
  vl::Result sync(size_t seg) override {
    (void)seg;
    if (refuse()) return vl::Result::Rejected;
    ++syncs;
    unsyncedFrom[seg] = size;
    return vl::Result::Ok;
  }

 private:
  std::vector<size_t> unsyncedFrom;  // per segment: first byte written since the last sync

  void powerFail() {
    budget = 0;
    dead = true;
    if (!loseUnsynced) return;
    for (size_t seg = 0; seg < count; ++seg) {
      if (unsyncedFrom[seg] < size) memset(&mem[seg * size + unsyncedFrom[seg]], 0xFF, size - unsyncedFrom[seg]);
      unsyncedFrom[seg] = size;
    }
  }

  bool refuse() {
    if (failNext) {
      failNext = false;
      return true;
    }
    return dead;
  }
};

}  // namespace vltest
