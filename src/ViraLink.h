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
#include "vl/api/Health.h"
#include "vl/api/Ota.h"
#include "vl/api/Persistence.h"
#include "vl/api/Provision.h"
#include "vl/api/Rpc.h"
#include "vl/core/JsonReader.h"
#include "vl/core/JsonWriter.h"
#include "vl/pal/FileOutboxStore.h"
#include "vl/se/Atecc608.h"
#include "vl/transport/AteccTlsStream.h"
#include "vl/transport/StreamMqtt.h"

#if defined(ARDUINO)
#include "vl/se/WireI2c.h"
#include "vl/transport/ArduinoStream.h"
#endif

#if VL_PLATFORM_ESP32
#include "vl/platform/esp32/EspMqtt.h"
#include "vl/platform/esp32/EspNvs.h"
#include "vl/platform/esp32/EspOta.h"
#endif
