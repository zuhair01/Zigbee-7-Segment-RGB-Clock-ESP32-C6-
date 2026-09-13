# Top-level project CMake for the Zigbee 7-segment RGB clock (ESP32-C6)
# Requires ESP-IDF v5.3+ and the esp-zigbee-sdk / esp-zboss-lib managed components.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(zigbee_7seg_clock)
