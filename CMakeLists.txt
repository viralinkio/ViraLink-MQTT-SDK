# Two modes:
#  - ESP-IDF component: picked up by idf.py when this directory is listed in
#    EXTRA_COMPONENT_DIRS or placed under components/ (also used when
#    arduino-esp32 runs as an IDF component).
#  - Host build (plain CMake): a static library for IDE indexing and local
#    checks. The unit tests run with `pio test -e native`.

if(ESP_PLATFORM)
  # SRC_DIRS is not recursive: list every source directory.
  idf_component_register(
    SRC_DIRS "src/vl" "src/vl/api" "src/vl/core" "src/vl/net" "src/vl/pal" "src/vl/platform/esp32" "src/vl/se" "src/vl/transport"
    INCLUDE_DIRS "src"
    REQUIRES mqtt esp_timer esp_ringbuf
    PRIV_REQUIRES esp_system esp_netif esp_wifi mbedtls app_update esp_http_client nvs_flash)
  return()
endif()

cmake_minimum_required(VERSION 3.16)
project(viralink_mqtt_sdk CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB_RECURSE VL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp")
add_library(viralink STATIC ${VL_SOURCES})
target_include_directories(viralink PUBLIC src)
target_compile_options(viralink PRIVATE -Wall -Wextra -Werror)
