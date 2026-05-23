cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico_w CACHE STRING "Board type")
include(${CMAKE_CURRENT_LIST_DIR}/../../../pico_sdk_import.cmake)

project(ir_test C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

add_executable(ir_test main.c)

pico_enable_stdio_usb(ir_test 1)
pico_enable_stdio_uart(ir_test 0)

target_link_libraries(ir_test pico_stdlib)

pico_add_extra_outputs(ir_test)
