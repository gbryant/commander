# ── commander_pico_runner ─────────────────────────────────────────────────
# Owns main(), FreeRTOS task wiring, WiFi init, panic hooks, and platform
# config (FreeRTOSConfig.h, lwipopts.h).  External apps link this target and
# provide commander_config() / commander_setup() in their own source.

# Inject platform config headers into library targets that need them at
# compile time.  These live here rather than in platform/pico/ so external
# FetchContent consumers get them automatically through the runner.
target_include_directories(commander_hal_pico         PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_include_directories(commander_transport_telnet PRIVATE ${CMAKE_CURRENT_LIST_DIR})

add_library(commander_pico_runner STATIC
    runner.cpp
    BootselModule.cpp
)

target_include_directories(commander_pico_runner PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>  # FreeRTOSConfig.h, lwipopts.h
)

target_link_libraries(commander_pico_runner PUBLIC
    commander_core
    commander_hal_pico
    commander_transport_uart
    commander_transport_telnet
    commander_modules
    pico_cyw43_arch_lwip_sys_freertos
    pico_lwip_mdns
    hardware_watchdog
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)

add_library(commander::pico_runner ALIAS commander_pico_runner)
