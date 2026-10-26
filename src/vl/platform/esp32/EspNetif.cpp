#include "vl/platform/esp32/EspNetif.h"

#if VL_PLATFORM_ESP32

#include <string.h>

#include "esp_idf_version.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
#include "esp_netif_net_stack.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#endif

namespace vl {

namespace {

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
void setDefault(void* netif) { netif_set_default(static_cast<struct netif*>(netif)); }
#endif

}  // namespace

// Looked up on every call: an interface may be created after this object, or
// destroyed and created again (WiFi off and on).
bool EspNetif::up() {
  esp_netif_t* n = esp_netif_get_handle_from_ifkey(key_);
  if (!n || !esp_netif_is_netif_up(n)) return false;
  esp_netif_ip_info_t ip;
  return esp_netif_get_ip_info(n, &ip) == ESP_OK && ip.ip.addr != 0;
}

void EspNetif::select() {
  esp_netif_t* n = esp_netif_get_handle_from_ifkey(key_);
  if (!n) return;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  esp_netif_set_default_netif(n);  // stays the default until changed again
#else
  // IDF 4.4 has no API for it; lwIP's own, on the tcpip thread. esp_netif may
  // pick another default on a later link event, which is why the network set
  // selects again before every connect.
  void* impl = esp_netif_get_netif_impl(n);
  if (impl) tcpip_callback(setDefault, impl);
#endif
}

bool EspNetif::rssi(int16_t& dBm) {
  if (strcmp(key_, "WIFI_STA_DEF") != 0) return false;
  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
  dBm = ap.rssi;
  return true;
}

}  // namespace vl

#endif  // VL_PLATFORM_ESP32
