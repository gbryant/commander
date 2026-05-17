# ESP-IDF component — place this repo under your IDF project's components/ dir,
# or set EXTRA_COMPONENT_DIRS in the top-level CMakeLists.txt.
# Requires ESP-IDF v5.0+ for the new i2c_master driver.

cmake_minimum_required(VERSION 3.20)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(commander)
