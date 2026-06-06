cmake_minimum_required(VERSION 3.20)

# In-tree ESP32 demo: build against the esp32 runner component straight from this
# repo (consumers do the same via FetchContent — see `cmdr init <name> esp32`).
get_filename_component(COMMANDER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
list(APPEND EXTRA_COMPONENT_DIRS ${COMMANDER_ROOT}/runners/esp32)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(commander)
