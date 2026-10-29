#include "vl/pal/Platform.h"

#include <stdio.h>

#include "vl/Config.h"

#if VL_PLATFORM_ESP32
#include "esp_system.h"
#if __has_include("esp_mac.h")  // IDF 5 split these out of esp_system.h
#include "esp_mac.h"
#include "esp_random.h"
#endif
#elif VL_PLATFORM_ESP8266
#include <Arduino.h>
#include <user_interface.h>
#elif defined(ARDUINO)
#include <Arduino.h>
#else
#include <random>
#endif

namespace vl {

uint32_t platformRandom() {
#if VL_PLATFORM_ESP32
  return esp_random();
#elif VL_PLATFORM_ESP8266
  return ESP.random();  // hardware RNG
#elif defined(ARDUINO)
  static bool seeded = false;
  if (!seeded) {
    randomSeed(analogRead(0) ^ micros());
    seeded = true;
  }
  return (static_cast<uint32_t>(random(0x10000)) << 16) ^ static_cast<uint32_t>(random(0x10000));
#else
  static std::random_device rd;
  return rd();
#endif
}

void platformClientId(char* out, size_t cap) {
  uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
#if VL_PLATFORM_ESP32
  esp_efuse_mac_get_default(mac);
#elif VL_PLATFORM_ESP8266
  wifi_get_macaddr(STATION_IF, mac);
#else
  uint32_t a = platformRandom(), b = platformRandom();
  for (int i = 0; i < 4; ++i) mac[i] = static_cast<uint8_t>(a >> (8 * i));
  mac[4] = static_cast<uint8_t>(b);
  mac[5] = static_cast<uint8_t>(b >> 8);
#endif
  snprintf(out, cap, "vl-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

}  // namespace vl
