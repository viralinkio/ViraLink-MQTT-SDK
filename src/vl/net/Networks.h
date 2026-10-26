// Several networks to the broker (WiFi, Ethernet, GSM) in priority order,
// with failover and fail-back. Platform-free; adapters for the networks live
// in the platform layer (EspNetif) or come from the application (LinkNetwork).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vl/Result.h"
#include "vl/pal/Clock.h"
#include "vl/pal/SystemInfo.h"
#include "vl/transport/IMqttTransport.h"

namespace vl {

// A network the client can reach the broker through. Used on the client task.
class INetwork {
 public:
  virtual ~INetwork() {}
  // For logs and the vl_iface attribute: "wifi", "eth", "gsm" (at most 8 characters).
  virtual const char* name() const = 0;
  // Link health: associated or registered, with an address. Must be cheap
  // and never block: it is sampled on every loop().
  virtual bool up() = 0;
  // The transport that reaches the broker over this network. Networks may
  // share one (WiFi and Ethernet through one EspMqtt).
  virtual IMqttTransport& transport() = 0;
  // Called on a move to this network and before every connect through it:
  // make it the default route.
  virtual void select() {}
  // Signal strength in dBm, where the network has one.
  virtual bool rssi(int16_t& dBm) {
    (void)dBm;
    return false;
  }
};

// INetwork from parts: a name, a transport and a function that tells whether
// the link is up, e.g. for a TinyGSM modem (modem.isGprsConnected()). A check
// that is slow, such as an AT command, runs at most every checkMs and the
// answer is reused in between. clock null: the platform clock.
class LinkNetwork : public INetwork {
 public:
  typedef bool (*UpFn)(void* ctx);
  LinkNetwork(const char* name, IMqttTransport& transport, UpFn up, void* ctx = nullptr, uint32_t checkMs = 0,
              IClock* clock = nullptr)
      : name_(name), transport_(transport), up_(up), ctx_(ctx), checkMs_(checkMs),
        clock_(clock ? clock : &platformClock()) {}
  const char* name() const override { return name_; }
  bool up() override;
  IMqttTransport& transport() override { return transport_; }

 private:
  const char* name_;
  IMqttTransport& transport_;
  UpFn up_;
  void* ctx_;
  uint32_t checkMs_;
  IClock* clock_;
  bool checked_ = false;
  bool last_ = false;
  uint64_t lastCheck_ = 0;
};

// The transport the client uses when it has several networks: it forwards to
// the transport of the active network and picks that network itself.
//   - The best network that is up is used; when it goes down the session
//     moves to the next one at once (failover).
//   - A better network must stay up failBackMs without a break before the
//     session moves back to it, so a flapping link does not bounce it.
//   - A network that is up but has let the session try for stallMs without
//     a single CONNACK (WiFi without uplink) is left for the next one that is
//     up, and sits out retryMs before fail-back may pick it again.
// A move while connected closes the link; the session reconnects through the
// new network and resends unacknowledged records.
//
// Also an ISystemInfo: vl_iface and vl_rssi describe the active network
// (health.setSource(nets)); heap and reset reason come from the platform.
class NetworkSet : public IMqttTransport, public ISystemInfo {
 public:
  struct Timing {
    uint32_t failBackMs = 30000;
    uint32_t stallMs = 120000;
    uint32_t retryMs = 300000;
  };
  // from or to is null for "no network".
  typedef void (*ChangeCb)(const char* from, const char* to, void* ctx);

  NetworkSet(const NetworkSet&) = delete;
  NetworkSet& operator=(const NetworkSet&) = delete;

  // Best first; before Client::begin(). NoMemory when the set is full.
  Result add(INetwork& net);
  void setTiming(const Timing& t) { timing_ = t; }
  void onChange(ChangeCb cb, void* ctx) {
    changeCb_ = cb;
    changeCtx_ = ctx;
  }
  // The network in use, or null.
  INetwork* active() const { return active_ < 0 ? nullptr : slots_[active_].net; }

  // IMqttTransport: every transport must accept the options, and the largest
  // packet is the smallest any of them takes.
  Result validate(const ConnectOptions& o) const override;
  bool linkUp() const override { return anyUp_; }
  void beginConnect(const ConnectOptions& opts) override;
  void disconnect() override;
  bool connected() const override;
  bool publish(const char* topic, const uint8_t* p, size_t n, uint8_t qos, uint16_t* packetId) override;
  void subscribe(const char* filter, uint8_t qos) override;
  void loop() override;
  size_t maxPacket() const override;
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

  // ISystemInfo
  bool heapFree(uint32_t& bytes) override;
  bool heapMin(uint32_t& bytes) override;
  const char* resetReason() override;
  const char* iface() override;
  bool rssi(int16_t& dBm) override;

 protected:
  struct Slot {
    INetwork* net;
    NetworkSet* owner;
    uint64_t upSince;       // while up
    uint64_t penaltyUntil;  // after a stall
    bool up;
  };
  NetworkSet(Slot* slots, size_t cap, IClock* clock);

 private:
  IMqttTransport& transportOf(int i) const { return slots_[i].net->transport(); }
  bool firstWith(size_t i) const;  // slot i is the first one using its transport
  bool fromActive(const Slot* s) const;
  bool penalised(size_t i, uint64_t now) const { return now < slots_[i].penaltyUntil; }
  int best(uint64_t now, int except, bool allowPenalised) const;
  void evaluate(uint64_t now);
  void switchTo(int target);

  static void thunkConnack(uint8_t rc, void* ctx);
  static void thunkSuback(const char* filter, bool granted, void* ctx);
  static void thunkPuback(uint16_t packetId, void* ctx);
  static void thunkMessage(const char* topic, const uint8_t* p, size_t n, void* ctx);
  static void thunkClosed(void* ctx);

  Slot* slots_;
  size_t cap_;
  size_t count_ = 0;
  IClock* clock_;
  Timing timing_;
  int active_ = -1;
  bool anyUp_ = false;
  bool engaged_ = false;        // a connection was started through the active transport and not dropped
  bool closedPending_ = false;  // report closed from loop()
  bool trying_ = false;         // connecting through the active network, no CONNACK yet
  uint64_t tryingSince_ = 0;
  uint64_t lastAttempt_ = 0;

  ChangeCb changeCb_ = nullptr;
  void* changeCtx_ = nullptr;
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

// NetworkSet for up to N networks. clock null: the platform clock.
template <size_t N>
class Networks : public NetworkSet {
 public:
  explicit Networks(IClock* clock = nullptr) : NetworkSet(slots_, N, clock) {}

 private:
  Slot slots_[N];
};

}  // namespace vl
