# ── commander_pico_runner ─────────────────────────────────────────────────
# Owns main(), FreeRTOS task wiring, WiFi init, panic hooks, and platform
# config (FreeRTOSConfig.h, lwipopts.h).  External apps link this target and
# provide commander_config() / commander_setup() in their own source.

# Inject platform config headers into library targets that need them at
# compile time.  These live here rather than in platform/pico/ so external
# FetchContent consumers get them automatically through the runner.
target_include_directories(commander_hal_pico         PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_include_directories(commander_transport_telnet PRIVATE ${CMAKE_CURRENT_LIST_DIR})

# ── commander_pico_ir ─────────────────────────────────────────────────────────
# IR receive on Pico (PIO + core1). The generated ir_rx.pio.h is needed only by
# PicoIRModule.cpp, so it's encapsulated here — PicoIRModule.h stays clean and
# the cmdr-generated commander_modules.h can include it with no PIO wiring. Lives
# in the runner dir so it picks up FreeRTOSConfig.h. Linked into the runner so
# enabling IR is pure registration (no per-consumer CMake injection); unused IR
# code is dropped at link by --gc-sections when no module instantiates it.
add_library(commander_pico_ir STATIC
    ${COMMANDER_ROOT}/platform/pico/PicoIRModule.cpp
)
pico_generate_pio_header(commander_pico_ir ${COMMANDER_ROOT}/platform/pico/ir_rx.pio)
target_include_directories(commander_pico_ir
    PUBLIC  $<BUILD_INTERFACE:${COMMANDER_ROOT}>
    PRIVATE ${CMAKE_CURRENT_LIST_DIR}        # FreeRTOSConfig.h
)
target_link_libraries(commander_pico_ir PUBLIC
    commander_core
    commander_hal_pico
    pico_stdlib
    hardware_pio
    pico_multicore
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)
add_library(commander::pico_ir ALIAS commander_pico_ir)

# ── commander_pico_ws2812 ─────────────────────────────────────────────────────
# Addressable-RGB output on Pico. Same encapsulation as commander_pico_ir: the
# generated ws2812.pio.h is needed only by the .cpp, so PicoWs2812Module.h stays
# free of PIO types and the cmdr-generated file can include it directly. Linked
# into the runner so enabling the module is pure registration; --gc-sections drops
# it when nothing instantiates it.
add_library(commander_pico_ws2812 STATIC
    ${COMMANDER_ROOT}/platform/pico/PicoWs2812Module.cpp
)
pico_generate_pio_header(commander_pico_ws2812 ${COMMANDER_ROOT}/platform/pico/ws2812.pio)
target_include_directories(commander_pico_ws2812
    PUBLIC  $<BUILD_INTERFACE:${COMMANDER_ROOT}>
    PRIVATE ${CMAKE_CURRENT_LIST_DIR}        # FreeRTOSConfig.h
)
target_link_libraries(commander_pico_ws2812 PUBLIC
    commander_core
    commander_hal_pico
    pico_stdlib
    hardware_pio
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)
add_library(commander::pico_ws2812 ALIAS commander_pico_ws2812)

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
    commander_pico_ir
    commander_pico_ws2812
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

# ── Controller / Bluetooth (optional) ──────────────────────────────────────────
# Bluepad32 + BTstack game-controller input. Heavy (BT firmware blob), so opt-in:
# `cmdr module enable controller` sets COMMANDER_ENABLE_CONTROLLER=ON (here) and
# add_compile_definitions(CYW43_ENABLE_BLUETOOTH=1) in the app CMake (must precede
# pico_sdk_init). Needs BLUEPAD32_PATH (a clone of ricardoquesada/bluepad32).
# btstack_config.h / sdkconfig.h ship in platform/pico/.
option(COMMANDER_ENABLE_CONTROLLER "Enable Bluepad32 controller backend (requires BLUEPAD32_PATH)" OFF)

if(COMMANDER_ENABLE_CONTROLLER)
    if(NOT DEFINED ENV{BLUEPAD32_PATH})
        message(FATAL_ERROR "COMMANDER_ENABLE_CONTROLLER=ON but BLUEPAD32_PATH is not set "
                            "(clone ricardoquesada/bluepad32 and export BLUEPAD32_PATH)")
    endif()
    set(_bp32_root $ENV{BLUEPAD32_PATH})
    set(_btstack_root ${PICO_SDK_PATH}/lib/btstack)

    # Configs + headers that btstack/bluepad32 compile against.
    include_directories(
        ${COMMANDER_ROOT}/platform/pico            # btstack_config.h, sdkconfig.h, bp32_pico.h
        ${CMAKE_CURRENT_LIST_DIR}                   # FreeRTOSConfig.h, lwipopts.h
        ${_btstack_root}/src
        ${_btstack_root}/3rd-party/bluedroid/encoder/include
        ${_btstack_root}/3rd-party/bluedroid/decoder/include
    )
    include_directories($<TARGET_PROPERTY:FreeRTOS-Kernel,INTERFACE_INCLUDE_DIRECTORIES>)

    add_subdirectory(${_bp32_root}/src/components/bluepad32 libbluepad32)
    # Bluepad32 hardcodes the FULL pico_cyw43_arch_none; the cyw43_arch ".c" files
    # are INTERFACE sources, so a static lib linking a full flavor recompiles and
    # duplicates them and collides with the runner's lwip_sys_freertos flavor
    # (threadsafe_background → "requires lwIP NO_SYS=1" #error). Repoint Bluepad32
    # at the HEADERS-only target; only the executable links the full flavor.
    foreach(_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(_libs bluepad32 ${_prop})
        if(_libs)
            list(REMOVE_ITEM _libs pico_cyw43_arch_none)
            list(APPEND _libs pico_cyw43_arch_lwip_sys_freertos_headers FreeRTOS-Kernel)
            set_target_properties(bluepad32 PROPERTIES ${_prop} "${_libs}")
        endif()
    endforeach()

    add_library(commander_pico_controller STATIC
        ${COMMANDER_ROOT}/platform/pico/bp32_pico.c
    )
    target_include_directories(commander_pico_controller
        PUBLIC  $<BUILD_INTERFACE:${COMMANDER_ROOT}>
        PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${COMMANDER_ROOT}/platform/pico
    )
    target_link_libraries(commander_pico_controller PUBLIC
        commander_core
        pico_stdlib
        pico_btstack_classic
        pico_btstack_ble
        pico_btstack_cyw43
        bluepad32
    )

    target_compile_definitions(commander_pico_runner PUBLIC COMMANDER_ENABLE_CONTROLLER)
    target_link_libraries(commander_pico_runner PUBLIC commander_pico_controller)
    # pico_btstack_* INTERFACE sources get compiled into the *app executable* too,
    # so btstack_config.h / sdkconfig.h (platform/pico) and the btstack 3rd-party
    # headers must reach the exe — propagate them PUBLIC through the runner.
    target_include_directories(commander_pico_runner PUBLIC
        ${COMMANDER_ROOT}/platform/pico
        ${_btstack_root}/3rd-party/bluedroid/encoder/include
        ${_btstack_root}/3rd-party/bluedroid/decoder/include
    )
endif()
