#!/usr/bin/env bash
# Builds every example against the library in this checkout:
#   - Arduino sketches with PlatformIO (framework = arduino), on each board
#     the sketch lists in its "// Boards:" line (esp32dev when it has none),
#     with the registry libraries of its "// Libraries:" line
#   - the ESP-IDF example with PlatformIO (framework = espidf)
#   - the ESP-IDF example with idf.py, if an ESP-IDF environment is active
# BOARDS="nodemcuv2" limits the Arduino builds to those boards.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BOARDS="${BOARDS:-}"
IDF_BOARDS="${IDF_BOARDS:-esp32dev}"
# Platform per framework; override to test other core / ESP-IDF releases.
ARDUINO_PLATFORM="${ARDUINO_PLATFORM:-espressif32}"
ESP8266_PLATFORM="${ESP8266_PLATFORM:-espressif8266}"
STM32_PLATFORM="${STM32_PLATFORM:-ststm32}"
IDF_PLATFORM="${IDF_PLATFORM:-espressif32@6.6.0}"  # ESP-IDF 5.2
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

platform_for() {
  case "$1" in
    nodemcu* | d1_mini* | esp01* | esp12e | esp8285 | huzzah) echo "$ESP8266_PLATFORM" ;;
    nucleo_* | bluepill* | blackpill* | genericSTM32*) echo "$STM32_PLATFORM" ;;
    *) echo "$ARDUINO_PLATFORM" ;;
  esac
}

for ino in "$ROOT"/examples/*/*.ino; do
  targets="$(sed -n 's#^// Boards: ##p' "$ino" | head -n 1)"
  libs="$(sed -n 's#^// Libraries: ##p' "$ino" | head -n 1 | tr ' ' ',')"
  for board in ${targets:-esp32dev}; do
    if [ -n "$BOARDS" ] && [[ " $BOARDS " != *" $board "* ]]; then continue; fi
    echo "== arduino $board $(basename "$ino")"
    pio ci "$ino" --lib "$ROOT" --board "$board" -O "platform=$(platform_for "$board")" -O "framework=arduino" \
      -O "build_flags=-Wall -Werror" ${libs:+-O "lib_deps=$libs"}
  done
done

for board in $IDF_BOARDS; do
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
