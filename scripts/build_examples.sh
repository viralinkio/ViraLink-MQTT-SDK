#!/usr/bin/env bash
# Builds every example against the library in this checkout:
#   - Arduino sketches with PlatformIO (framework = arduino), each for the
#     boards of its target: ESP32 (BOARDS), or ESP8266 and AVR (PORT_BOARDS)
#   - the ESP-IDF example with PlatformIO (framework = espidf)
#   - the ESP-IDF example with idf.py, if an ESP-IDF environment is active
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BOARDS="${BOARDS:-esp32dev}"
PORT_BOARDS="${PORT_BOARDS:-nodemcuv2 megaatmega2560}"
# Platform per framework; override to test other arduino-esp32 / ESP-IDF releases.
ARDUINO_PLATFORM="${ARDUINO_PLATFORM:-espressif32}"
IDF_PLATFORM="${IDF_PLATFORM:-espressif32@6.6.0}"  # ESP-IDF 5.2
"$ROOT/scripts/version.sh" check >/dev/null  # the three manifests must agree before anything ships
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

platform_for() {  # board -> PlatformIO platform
  case "$1" in
    nodemcu* | d1_mini* | esp01* | esp12e) echo espressif8266 ;;
    mega* | uno | nano*) echo atmelavr ;;
    nucleo_* | blackpill_* | genericSTM32*) echo ststm32 ;;
    *) echo "$ARDUINO_PLATFORM" ;;
  esac
}
target_of() {  # example -> the platform it is written for
  case "$1" in
    esp8266) echo espressif8266 ;;
    lite_avr) echo atmelavr ;;
    *) echo espressif32 ;;
  esac
}

for board in $BOARDS $PORT_BOARDS; do
  platform="$(platform_for "$board")"
  for ino in "$ROOT"/examples/*/*.ino; do
    [ "$(target_of "$(basename "$(dirname "$ino")")")" = "${platform%%@*}" ] || continue
    echo "== arduino $board $(basename "$ino")"
    # ArduinoJson only for compat_v1 (ViraLinkCompat.h); the library needs none.
    pio ci "$ino" --lib "$ROOT" --board "$board" -O "platform=$platform" -O "framework=arduino" \
      -O "build_flags=-Wall -Werror" -O "lib_deps=bblanchon/ArduinoJson@^7.4.2"
  done
done

for board in $BOARDS; do
  echo "== espidf (pio) $board espidf_basic_telemetry"
  proj="$WORK/espidf-$board"
  mkdir -p "$proj/src"
  cp "$ROOT/examples/espidf_basic_telemetry/main/main.cpp" "$proj/src/"
  cp "$ROOT/examples/espidf_basic_telemetry/sdkconfig.defaults" "$proj/"
  cat > "$proj/platformio.ini" <<INI
[env:$board]
platform = $IDF_PLATFORM
board = $board
framework = espidf
lib_deps = symlink://$ROOT
INI
  cat > "$proj/CMakeLists.txt" <<'CM'
cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(vl_espidf_example)
CM
  printf 'idf_component_register(SRCS "main.cpp")\n' > "$proj/src/CMakeLists.txt"
  (cd "$proj" && pio run)
done

if command -v idf.py >/dev/null 2>&1; then
  echo "== espidf (idf.py) espidf_basic_telemetry"
  cp -r "$ROOT/examples/espidf_basic_telemetry" "$WORK/idf"
  sed -i "s#\${CMAKE_CURRENT_LIST_DIR}/../..#$ROOT#" "$WORK/idf/CMakeLists.txt"
  (cd "$WORK/idf" && idf.py build)
else
  echo "== idf.py not on PATH; skipped the native ESP-IDF build"
fi
