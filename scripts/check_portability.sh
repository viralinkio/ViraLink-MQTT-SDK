#!/usr/bin/env bash
# Checks that the library builds for every class of microcontroller it targets.
#   1. Platform-free sources compiled alone by each cross compiler with
#      -Wall -Wextra -Werror: 8-bit AVR (16-bit int, 32-bit double), Cortex-M0+,
#      ESP8266 (Xtensa LX106) and ESP32-C3 (RISC-V).
#   2. test/portability/portability.ino linked through the Arduino core of each
#      board in PORT_BOARDS: catches Arduino.h macro clashes, platform-layer and
#      link errors, and RAM overflow.
# Uses the toolchains PlatformIO caches; a missing one is reported and skipped.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages"
BOARDS="${PORT_BOARDS:-megaatmega2560 nucleo_f401re nodemcuv2}"
FLAGS=(-std=gnu++11 -Os -Wall -Wextra -Werror -fno-exceptions -fno-rtti "-I$ROOT/src")
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

shopt -s nullglob
SOURCES=("$ROOT"/src/vl/core/*.cpp "$ROOT"/src/vl/api/*.cpp "$ROOT"/src/vl/transport/*.cpp "$ROOT/src/vl/Client.cpp" "$ROOT/src/vl/Log.cpp")

compile() {  # name compiler [target flags...]
  local name=$1 cc=$2
  shift 2
  if [ ! -x "$cc" ]; then
    echo "== $name: $(basename "$cc") not installed, skipped"
    return
  fi
  echo "== $name ($("$cc" -dumpversion))"
  for f in "${SOURCES[@]}"; do
    "$cc" "$@" "${FLAGS[@]}" -c "$f" -o "$WORK/$name-$(basename "$f").o"
  done
}

compile avr "$PKG/toolchain-atmelavr/bin/avr-g++" -mmcu=atmega2560
compile cortex-m0plus "$PKG/toolchain-gccarmnoneeabi/bin/arm-none-eabi-g++" -mcpu=cortex-m0plus -mthumb
compile esp8266 "$PKG/toolchain-xtensa/bin/xtensa-lx106-elf-g++" -mlongcalls
compile esp32c3 "$PKG/toolchain-riscv32-esp/bin/riscv32-esp-elf-g++" -march=rv32imc -mabi=ilp32

for board in $BOARDS; do
  echo "== arduino $board"
  # The library does not advertise these platforms yet, hence lib_compat_mode.
  pio ci "$ROOT/test/portability/portability.ino" --lib "$ROOT" --board "$board" -O "lib_compat_mode=off"
done
