// ViraLink Device Client v2: ThingsBoard-compatible MQTT device client.
// Works with Arduino IDE, PlatformIO (Arduino and ESP-IDF frameworks) and
// ESP-IDF (as a component).
#pragma once

#include "vl/Client.h"
#include "vl/Config.h"
#include "vl/Log.h"
#include "vl/Result.h"
#include "vl/api/Attributes.h"
#include "vl/api/Gateway.h"
#include "vl/api/Ota.h"
#include "vl/api/Provision.h"
#include "vl/api/Rpc.h"
#include "vl/core/JsonReader.h"
#include "vl/core/JsonWriter.h"

#if VL_PLATFORM_ESP32
#include "vl/platform/esp32/EspMqtt.h"
#include "vl/platform/esp32/EspNvs.h"
#include "vl/platform/esp32/EspOta.h"
#endif
