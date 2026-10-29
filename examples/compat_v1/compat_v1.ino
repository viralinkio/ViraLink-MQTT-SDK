// A ViraLink-MQTT-Client 1.0.2 sketch on v2 through ViraLinkCompat.h: the only
// change from 1.0.2 is the #include. Needs ArduinoJson 7, as 1.0.2 did.
//
// Underneath, records wait in the v2 outbox until the broker acknowledges
// them, telemetry is stamped when queued, and OTA takes any checksum
// algorithm. New sketches should use vl::Client directly (see
// basic_telemetry); docs/MIGRATING.md maps every 1.0.2 call.

#include <WiFi.h>
#include <ViraLinkCompat.h>  // was: #include "viralink.h"

#define FIRMWARE_TITLE "viralink-test"
#define FIRMWARE_VERSION "1"

static const char* WIFI_SSID = "your-ssid";
static const char* WIFI_PASS = "your-password";
static const char* VL_TOKEN = "YOUR_DEVICE_TOKEN";

WiFiClient wifiClient;
MQTTController mqttController;
MQTTOTA ota(&mqttController, 10240);
unsigned long lastSend = 0;

bool onMessage(const String& topic, JsonDocument json) {
  Serial.println("Topic: " + topic);
  if (topic.startsWith("v1/devices/me/rpc/request/") && json["method"] == "getUptime") {
    String id = topic.substring(strlen("v1/devices/me/rpc/request/"));
    JsonDocument reply;
    reply["uptime"] = millis() / 1000;
    String payload;
    serializeJson(reply, payload);
    mqttController.addToPublishQueue("v1/devices/me/rpc/response/" + id, payload);
  }
  return true;
}

bool onSharedAttributes(String topic, JsonDocument json) {
  Serial.println("Shared attributes: " + json["shared"].as<String>());
  return true;
}

void onConnected() {
  Serial.println("Connected To Platform");
  JsonDocument data;
  data["Connection Type"] = "WIFI";
  data["IP"] = WiFi.localIP().toString();
  mqttController.sendAttributes(data);
  mqttController.requestAttributesJson("{\"sharedKeys\":\"interval\"}", onSharedAttributes);
}

void setup() {
  Serial.begin(115200);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  mqttController.init();
  mqttController.sendSystemAttributes(true);
  ota.begin(FIRMWARE_TITLE, FIRMWARE_VERSION);
  mqttController.connect(wifiClient, "esp", VL_TOKEN, "", "console.viralink.io", 1883, onMessage, nullptr,
                         onConnected);
}

void loop() {
  mqttController.loop();
  if (millis() - lastSend > 10000) {
    lastSend = millis();
    JsonDocument data;
    data["temperature"] = temperatureRead();
    mqttController.sendTelemetry(data);
  }
  delay(5);
}
