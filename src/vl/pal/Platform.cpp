#include "vl/pal/Platform.h"

#include <stdio.h>

#include "vl/Config.h"

#if VL_PLATFORM_ESP32
#include "esp_system.h"
#if __has_include("esp_mac.h")  // IDF 5 split these out of esp_system.h
#include "esp_mac.h"
#include "esp_random.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#elif VL_PLATFORM_ESP8266
#include <Arduino.h>
#include <user_interface.h>
#elif VL_PLATFORM_STM32
#include <Arduino.h>
#elif defined(ARDUINO)
#include <Arduino.h>
#else
#include <random>
#endif

namespace vl {

#if VL_PLATFORM_STM32
namespace {

void stm32Uid(uint32_t w[3]) {
  w[0] = HAL_GetUIDw0();
  w[1] = HAL_GetUIDw1();
  w[2] = HAL_GetUIDw2();
}

// The RNG peripheral, where the chip has one and its clock runs.
bool stm32HardwareRandom(uint32_t& out) {
#if defined(RNG) && defined(__HAL_RCC_RNG_CLK_ENABLE)
  static bool started = false;
  if (!started) {
    __HAL_RCC_RNG_CLK_ENABLE();
    RNG->CR |= RNG_CR_RNGEN;
    started = true;
  }
  for (int i = 0; i < 2000; ++i) {
    uint32_t sr = RNG->SR;
    if (sr & (RNG_SR_SECS | RNG_SR_CECS)) return false;  // seed or clock error
    if (sr & RNG_SR_DRDY) {
      out = RNG->DR;
      return true;
    }
  }
#else
  (void)out;
#endif
  return false;
}

}  // namespace
#endif

uint32_t platformRandom() {
#if VL_PLATFORM_ESP32
  return esp_random();
#elif VL_PLATFORM_ESP8266
  return RANDOM_REG32;  // hardware RNG
#elif VL_PLATFORM_STM32
  uint32_t v;
  if (stm32HardwareRandom(v)) return v;
  // xorshift32 seeded from the unique ID: enough for devices to differ.
  static uint32_t s = 0;
  if (!s) {
    uint32_t w[3];
    stm32Uid(w);
    s = w[0] ^ (w[1] * 2654435761u) ^ (w[2] * 40503u) ^ micros();
    if (!s) s = 1;
  }
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
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

void platformYield() {
#if VL_PLATFORM_ESP32
  vTaskDelay(1);
#elif defined(ARDUINO)
  yield();
#endif
}

void platformClientId(char* out, size_t cap) {
  uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
#if VL_PLATFORM_ESP32
  esp_efuse_mac_get_default(mac);
#elif VL_PLATFORM_ESP8266
  wifi_get_macaddr(STATION_IF, mac);
#elif VL_PLATFORM_STM32
  uint32_t w[3];
  stm32Uid(w);  // 96 bits folded to 48
  uint32_t a = w[0] ^ w[2];
  for (int i = 0; i < 4; ++i) mac[i] = static_cast<uint8_t>(a >> (8 * i));
  mac[4] = static_cast<uint8_t>(w[1] ^ (w[1] >> 16));
  mac[5] = static_cast<uint8_t>((w[1] >> 8) ^ (w[1] >> 24));
#else
  uint32_t a = platformRandom(), b = platformRandom();
  for (int i = 0; i < 4; ++i) mac[i] = static_cast<uint8_t>(a >> (8 * i));
  mac[4] = static_cast<uint8_t>(b);
  mac[5] = static_cast<uint8_t>(b >> 8);
#endif
  snprintf(out, cap, "vl-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

}  // namespace vl
