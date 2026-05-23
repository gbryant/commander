cmake_minimum_required(VERSION 3.20)

set(PICO_BOARD pico_w CACHE STRING "Board type")
include(${CMAKE_CURRENT_LIST_DIR}/../../pico_sdk_import.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../../FreeRTOS_Kernel_import.cmake)

project(commander C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()


execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(
    COMMAND git rev-list --count HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_BUILD_NUMBER OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT GIT_COMMIT)
    set(GIT_COMMIT "unknown")
endif()
if(NOT GIT_BUILD_NUMBER)
    set(GIT_BUILD_NUMBER 0)
endif()

add_executable(commander
    main.cpp
    BootselModule.cpp
    ../../core/CommandRegistry.cpp
    ../../hal/pico/hal.cpp
    ../../transport/uart/UartTransport.cpp
    ../../transport/telnet/TelnetTransport.cpp
)

pico_generate_pio_header(commander ${CMAKE_CURRENT_LIST_DIR}/ir_rx.pio)

target_compile_definitions(commander PRIVATE
    CYW43_TASK_STACK_SIZE=2048
    BUILD_COMMIT="${GIT_COMMIT}"
    BUILD_NUMBER=${GIT_BUILD_NUMBER}
)

target_include_directories(commander PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}       # FreeRTOSConfig.h
    ${CMAKE_CURRENT_LIST_DIR}/../.. # project root: hal/, core/, modules/
    ${CMAKE_CURRENT_LIST_DIR}/../../include
)

target_link_libraries(commander
    pico_stdlib
    pico_cyw43_arch_lwip_sys_freertos
    pico_lwip_mdns
    hardware_i2c
    hardware_gpio
    hardware_pio
    hardware_clocks
    pico_multicore
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)

pico_enable_stdio_usb(commander 1)
pico_enable_stdio_uart(commander 0)
pico_add_extra_outputs(commander)
