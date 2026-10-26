// ESP-IDF version of basic_telemetry: WiFi station, SNTP, telemetry every 10 s
// from the client task.
#include <string.h>

#include "ViraLink.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-password"
#define VL_HOST "console.viralink.ir"
#define VL_TOKEN "YOUR_DEVICE_TOKEN"

static vl::EspMqtt mqtt;
static vl::RingOutbox<16 * 1024> outbox;
static vl::Client client(mqtt, outbox);

static void sendHeap(void*) {
  client.telemetry().add("heap", esp_get_free_heap_size()).add("heap_min", esp_get_minimum_free_heap_size()).send();
}

static void wifiStart() {
  esp_netif_init();
  esp_event_loop_create_default();
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&init);
  wifi_config_t wc = {};
  strncpy(reinterpret_cast<char*>(wc.sta.ssid), WIFI_SSID, sizeof(wc.sta.ssid));
  strncpy(reinterpret_cast<char*>(wc.sta.password), WIFI_PASS, sizeof(wc.sta.password));
  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_set_config(WIFI_IF_STA, &wc);
  // Reconnect WiFi whenever it drops; the client reconnects MQTT on its own.
  esp_event_handler_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID,
      [](void*, esp_event_base_t, int32_t id, void*) {
        if (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED) esp_wifi_connect();
      },
      nullptr);
  esp_wifi_start();
}

extern "C" void app_main() {
  if (nvs_flash_init() != ESP_OK) {
    nvs_flash_erase();
    nvs_flash_init();
  }
  wifiStart();
  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();

  client.onState([](vl::State s, void*) { ESP_LOGI("app", "state: %s", vl::toString(s)); }, nullptr);
  client.every(10000, sendHeap, nullptr);

  vl::Config cfg;
  cfg.host = VL_HOST;
  cfg.port = 1883;
  cfg.auth = vl::AccessToken(VL_TOKEN);
  client.begin(cfg);
  client.attributes().add("fw", "1.0.0").add("sdk", VL_SDK_VERSION).send();

  client.startTask(/*core*/ 0, /*priority*/ 5, /*stack*/ 6144);
}
