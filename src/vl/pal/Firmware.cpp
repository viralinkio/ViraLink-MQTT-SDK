#include "vl/pal/Firmware.h"

#include "vl/Config.h"

namespace vl {

#if !VL_PLATFORM_ESP32
// ESP32 defines these in platform/esp32/EspOta.cpp.
IFirmwareWriter* platformFirmwareWriter() { return nullptr; }
IHttpSource* platformHttpSource() { return nullptr; }
#endif

}  // namespace vl
