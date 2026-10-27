#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vl {

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF). Pass the previous result as
// crc to continue over another span.
uint16_t crc16(const uint8_t* p, size_t n, uint16_t crc = 0xFFFF);

}  // namespace vl
