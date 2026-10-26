# ESP-IDF example: idf.py build (from this directory).
# The library repository root is an ESP-IDF component.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(vl_basic_telemetry)
