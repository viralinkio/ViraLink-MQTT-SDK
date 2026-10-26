#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vl {

// Writes the default MQTT client ID ("vl-" + 12 hex digits of the MAC on
// ESP32, random elsewhere). Always 15 characters, within the 23-character
// MQTT 3.1.1 limit.
void platformClientId(char* out, size_t cap);

// Hardware entropy where available; seeds backoff jitter and the boot ID.
uint32_t platformRandom();

}  // namespace vl
