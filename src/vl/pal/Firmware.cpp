#include "vl/pal/Firmware.h"

#include "vl/Config.h"

namespace vl {

#if !VL_PLATFORM_ESP32 && !VL_PLATFORM_ESP8266
// ESP32 and ESP8266 define these in platform/<target>/.
IFirmwareWriter* platformFirmwareWriter() { return nullptr; }
IHttpSource* platformHttpSource() { return nullptr; }
#endif

}  // namespace vl
