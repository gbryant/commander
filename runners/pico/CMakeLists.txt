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

# ── OTA (optional) ────────────────────────────────────────────────────────────
# Requires pico_fota_bootloader to be added via add_subdirectory() by the app's
# CMakeLists before the build runs. Set COMMANDER_ENABLE_OTA=ON before
# FetchContent_MakeAvailable(commander) or add_subdirectory(commander).
option(COMMANDER_ENABLE_OTA "Enable OTA update command (requires pico_fota_bootloader)" OFF)

if(COMMANDER_ENABLE_OTA)
    target_compile_definitions(commander_pico_runner PUBLIC COMMANDER_ENABLE_OTA)
    target_link_libraries(commander_pico_runner PUBLIC
        hardware_flash
        hardware_sync
        pico_multicore
    )
    # PFB headers come from pico_fota_bootloader_lib, added by the app.
    # Generator expression is safe here — evaluated at generation time after all
    # add_subdirectory() calls have run, so the target is guaranteed to exist.
    target_include_directories(commander_pico_runner PRIVATE
        $<TARGET_PROPERTY:pico_fota_bootloader_lib,INTERFACE_INCLUDE_DIRECTORIES>
    )
endif()
