#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vl {

// Writes the default MQTT client ID ("vl-" + 12 hex digits of the MAC on
// ESP32 and ESP8266, random elsewhere). Always 15 characters, within the 23-character
// MQTT 3.1.1 limit.
void platformClientId(char* out, size_t cap);

// Hardware entropy where available; seeds backoff jitter and the boot ID.
uint32_t platformRandom();

// Called by library loops that poll while they block, so the ESP8266
// software watchdog and the ESP32 idle task get their turn.
void platformYield();

}  // namespace vl
