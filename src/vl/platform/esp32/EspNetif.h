#pragma once

#include "vl/Config.h"

#if VL_PLATFORM_ESP32

#include "vl/net/Networks.h"

namespace vl {

// INetwork over an ESP32 lwIP interface (Arduino and ESP-IDF), found by its
// key: "WIFI_STA_DEF" (WiFi station), "ETH_DEF" (Ethernet: LAN8720 and other
// PHYs), "PPP_DEF" (a PPPoS modem). Up means the interface is up with an IPv4
// address; select() makes it lwIP's default route, so one EspMqtt can serve
// every such interface. A connection stays on the interface it was made on.
//
// lwIP keeps one DNS server list for all interfaces: when they sit on
// different LANs, use a public resolver (or a broker address) that every one
// of them can reach.
class EspNetif : public INetwork {
 public:
  // name: for logs and vl_iface. Strings must outlive the object.
  EspNetif(const char* name, const char* ifKey, IMqttTransport& transport)
      : name_(name), key_(ifKey), transport_(transport) {}

  const char* name() const override { return name_; }
  bool up() override;
  IMqttTransport& transport() override { return transport_; }
  void select() override;
  bool rssi(int16_t& dBm) override;  // WiFi station only

 private:
  const char* name_;
  const char* key_;
  IMqttTransport& transport_;
};

}  // namespace vl

#endif  // VL_PLATFORM_ESP32
