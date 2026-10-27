#include "vl/core/Crc16.h"

namespace vl {

uint16_t crc16(const uint8_t* p, size_t n, uint16_t crc) {
  while (n--) {
    crc ^= static_cast<uint16_t>(*p++) << 8;
    for (int i = 0; i < 8; ++i) crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : crc << 1;
  }
  return crc;
}

}  // namespace vl
