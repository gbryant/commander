cmake_minimum_required(VERSION 3.20)

set(PICO_BOARD pico_w CACHE STRING "Board type")
include(${CMAKE_CURRENT_LIST_DIR}/../../pico_sdk_import.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../../FreeRTOS_Kernel_import.cmake)

project(commander C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(commander
    main.cpp
    ../../core/CommandRegistry.cpp
    ../../hal/pico/hal.cpp
    ../../transport/uart/UartTransport.cpp
)

target_include_directories(commander PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}       # FreeRTOSConfig.h
    ${CMAKE_CURRENT_LIST_DIR}/../.. # project root: hal/, core/, modules/
    ${CMAKE_CURRENT_LIST_DIR}/../../include
)

target_link_libraries(commander
    pico_stdlib
    pico_cyw43_arch_lwip_threadsafe_background
    hardware_i2c
    hardware_gpio
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)

pico_enable_stdio_usb(commander 1)
pico_enable_stdio_uart(commander 0)
pico_add_extra_outputs(commander)
