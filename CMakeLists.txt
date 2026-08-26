cmake_minimum_required(VERSION 3.20)

# ── Standalone build ──────────────────────────────────────────────────────────
# When consumed via FetchContent the caller already ran pico_sdk_init() and
# project(). Skip those here to avoid re-initialising the SDK.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(PICO_BOARD pico_w CACHE STRING "Board type")
    include(${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake)
    include(${CMAKE_CURRENT_LIST_DIR}/FreeRTOS_Kernel_import.cmake)
    project(commander C CXX ASM)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_CXX_STANDARD 17)
    pico_sdk_init()
endif()

set(COMMANDER_ROOT ${CMAKE_CURRENT_LIST_DIR})
include(${COMMANDER_ROOT}/cmake/GenerateScripts.cmake)

# ── commander_core ────────────────────────────────────────────────────────────
# Pure C++ — CommandRegistry, Writer, IModule. No platform dependencies.
add_library(commander_core STATIC
    ${COMMANDER_ROOT}/core/CommandRegistry.cpp
)
target_include_directories(commander_core PUBLIC
    $<BUILD_INTERFACE:${COMMANDER_ROOT}>
    $<BUILD_INTERFACE:${COMMANDER_ROOT}/include>
)
add_library(commander::core ALIAS commander_core)

# ── commander_modules ─────────────────────────────────────────────────────────
# Header-only sensor/actuator modules (CompassModule, SonarModule, …).
# Depend only on commander_core and hal.h.
add_library(commander_modules INTERFACE)
target_include_directories(commander_modules INTERFACE
    $<BUILD_INTERFACE:${COMMANDER_ROOT}>
)
target_link_libraries(commander_modules INTERFACE commander_core)
add_library(commander::modules ALIAS commander_modules)

# ── commander_hal_pico ────────────────────────────────────────────────────────
# Pico SDK HAL implementation.
# IMPORTANT: FreeRTOSConfig.h must be on the include path at compile time.
# In platform/pico/ builds it is added by platform/pico/CMakeLists.txt.
# In runner-based builds (step 2) the runner target owns FreeRTOSConfig.h.
add_library(commander_hal_pico STATIC
    ${COMMANDER_ROOT}/hal/pico/hal.cpp
)
target_include_directories(commander_hal_pico PUBLIC
    $<BUILD_INTERFACE:${COMMANDER_ROOT}>
    $<BUILD_INTERFACE:${COMMANDER_ROOT}/include>
)
target_link_libraries(commander_hal_pico PUBLIC
    commander_core
    pico_stdlib
    hardware_i2c
    hardware_gpio
    hardware_spi
    hardware_adc
    hardware_pwm
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)
add_library(commander::hal_pico ALIAS commander_hal_pico)

# ── commander_transport_uart ──────────────────────────────────────────────────
add_library(commander_transport_uart STATIC
    ${COMMANDER_ROOT}/transport/uart/UartTransport.cpp
)
target_include_directories(commander_transport_uart PUBLIC
    $<BUILD_INTERFACE:${COMMANDER_ROOT}>
)
target_link_libraries(commander_transport_uart PUBLIC
    commander_core
)
add_library(commander::transport_uart ALIAS commander_transport_uart)

# ── commander_transport_telnet ────────────────────────────────────────────────
# lwIP BSD-socket Telnet server (Pico/ESP32 targets).
# IMPORTANT: lwipopts.h must be on the include path at compile time.
# In platform/pico/ builds it is added by platform/pico/CMakeLists.txt.
# In runner-based builds (step 2) the runner target owns lwipopts.h.
add_library(commander_transport_telnet STATIC
    ${COMMANDER_ROOT}/transport/telnet/TelnetTransport.cpp
)
target_include_directories(commander_transport_telnet PUBLIC
    $<BUILD_INTERFACE:${COMMANDER_ROOT}>
)
target_link_libraries(commander_transport_telnet PUBLIC
    commander_core
    pico_cyw43_arch_lwip_sys_freertos
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)
add_library(commander::transport_telnet ALIAS commander_transport_telnet)

# ── Runners ───────────────────────────────────────────────────────────────────
# Each runner owns FreeRTOSConfig.h, lwipopts.h, and the platform entry (main()).
# External apps link a single runner target and provide commander_config() /
# commander_setup() in their own source.
add_subdirectory(${COMMANDER_ROOT}/runners/pico)
