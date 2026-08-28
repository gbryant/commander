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
)
# The heap is the application's choice, and a firmware that already picked one
# (debugprobe uses heap_1) must not have a second linked in underneath it. Only
# the runner-based build — where commander IS the application — selects heap_4.
if(NOT COMMANDER_LIBRARIES_ONLY)
    target_link_libraries(commander_hal_pico PUBLIC FreeRTOS-Kernel-Heap4)
endif()

# The console backend is likewise the application's: a firmware owning its own
# USB stack can put the shell (and every module stream) on its own endpoint by
# supplying hal_uart_* itself. See the note in hal/pico/hal.cpp.
option(COMMANDER_PICO_UART_EXTERNAL
       "The consumer supplies hal_uart_* instead of the SDK stdio console" OFF)
if(COMMANDER_PICO_UART_EXTERNAL)
    target_compile_definitions(commander_hal_pico PUBLIC COMMANDER_PICO_UART_EXTERNAL=1)
endif()
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
# Skipped under COMMANDER_LIBRARIES_ONLY: it links pico_cyw43_arch, which a board
# without a radio cannot satisfy.
if(NOT COMMANDER_LIBRARIES_ONLY)
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
endif()

# ── Runners ───────────────────────────────────────────────────────────────────
# Each runner owns FreeRTOSConfig.h, lwipopts.h, and the platform entry (main()).
# External apps link a single runner target and provide commander_config() /
# commander_setup() in their own source.
#
# COMMANDER_LIBRARIES_ONLY skips the runner entirely, for a firmware that already
# owns main(), its FreeRTOS configuration and its USB stack — embedding commander
# into an existing application rather than being built around it. cmdr-probe is
# the first: a debugprobe fork that hosts a commander console on a second USB CDC.
# Without this the runner drags in pico_cyw43_arch (WiFi) on boards that have no
# radio, and a second FreeRTOSConfig.h to fight the host's.
#
# Such a project links commander_core / commander_modules / commander_hal_pico /
# commander_transport_uart directly, and takes on the runner's CONTRACT: call
# commander_setup(reg), then commander_on_uart_ready(transport) so ticking modules
# are pumped, then commander_run_autostart(reg). Skip those and cmdr-managed
# modules will compile and quietly do nothing.
option(COMMANDER_LIBRARIES_ONLY
       "Build only the portable libraries — no runner, no main(), no radio" OFF)

if(COMMANDER_LIBRARIES_ONLY)
    # FreeRTOSConfig.h normally comes from the runner. Without one, the consumer
    # owns it (it configures ITS scheduler, so this is right) and must say where:
    #   set(COMMANDER_FREERTOS_CONFIG_DIR ${CMAKE_CURRENT_LIST_DIR}/src CACHE PATH "" FORCE)
    # before FetchContent_MakeAvailable(commander).
    if(NOT COMMANDER_FREERTOS_CONFIG_DIR)
        message(FATAL_ERROR
            "COMMANDER_LIBRARIES_ONLY needs COMMANDER_FREERTOS_CONFIG_DIR — the "
            "directory holding your FreeRTOSConfig.h. With no runner, commander's "
            "HAL has no scheduler configuration to compile against.")
    endif()
    target_include_directories(commander_hal_pico PRIVATE ${COMMANDER_FREERTOS_CONFIG_DIR})
else()
    add_subdirectory(${COMMANDER_ROOT}/runners/pico)
endif()
