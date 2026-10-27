#include "vl/pal/Firmware.h"

#include "vl/Config.h"

namespace vl {

// ESP32 defines both in platform/esp32/EspOta.cpp, ESP8266 the writer in
// platform/esp8266/Esp8266Ota.cpp.
#if !VL_PLATFORM_ESP32 && !VL_PLATFORM_ESP8266
IFirmwareWriter* platformFirmwareWriter() { return nullptr; }
#endif
#if !VL_PLATFORM_ESP32
IHttpSource* platformHttpSource() { return nullptr; }
#endif

}  // namespace vl
