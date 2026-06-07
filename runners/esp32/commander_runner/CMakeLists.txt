# commander_runner — ESP-IDF component
# Owns app_main(), WiFi init, FreeRTOS task wiring, and weak hook defaults.
# Consumer's main component provides commander_config() and commander_setup().
#
# Set COMMANDER_ROOT before including IDF in your CMakeLists.txt:
#   set(COMMANDER_ROOT ${commander_SOURCE_DIR})
# If not set, it is inferred from this file's location in the repo.

if(NOT DEFINED COMMANDER_ROOT)
    get_filename_component(COMMANDER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
endif()

set(CMDR_SRCS
    "runner.cpp"
    "${COMMANDER_ROOT}/core/CommandRegistry.cpp"
    "${COMMANDER_ROOT}/hal/esp32/hal.cpp"
    "${COMMANDER_ROOT}/transport/uart/UartTransport.cpp"
    "${COMMANDER_ROOT}/transport/telnet/TelnetTransport.cpp"
)
set(CMDR_REQUIRES
    esp_driver_i2c
    esp_driver_gpio
    esp_driver_usb_serial_jtag
    esp_driver_uart   # classic ESP32 / S2 console (no USB-Serial/JTAG peripheral)
    esp_timer
    freertos
    nvs_flash
    esp_netif
    esp_wifi
    esp_event
    app_update
    esp_http_client
    espressif__mdns
    # ipstube display driver deps. Unconditional: REQUIRES is resolved during
    # ESP-IDF's early-expansion phase, where a CMake cache var like
    # COMMANDER_ENABLE_IPSTUBE is NOT yet visible — a conditional append here is
    # silently dropped (the source compiles but the include path is missing).
    # gc-sections drops the unused code from non-display builds, so the only cost
    # is building these (small) components.
    esp_lcd
    esp_driver_spi
    esp_driver_ledc
)

# The display *source* is still gated — SRCS are read in the normal phase where
# COMMANDER_ENABLE_IPSTUBE is set, so only ipstube builds compile the driver.
if(COMMANDER_ENABLE_IPSTUBE)
    list(APPEND CMDR_SRCS "${COMMANDER_ROOT}/platform/esp32/IpstubeModule.cpp")
endif()

idf_component_register(
    SRCS ${CMDR_SRCS}
    INCLUDE_DIRS
        "${COMMANDER_ROOT}"
        "${COMMANDER_ROOT}/include"
        "${CMAKE_CURRENT_LIST_DIR}"
    REQUIRES ${CMDR_REQUIRES}
)

if(COMMANDER_ENABLE_OTA)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC COMMANDER_ENABLE_OTA=1)
endif()

# NOTE: version stamping (commander_build.h, see cmake/VersionStamp.cmake) is wired
# for PlatformIO (R4/Uno) and the pico CMake path, but NOT esp32 yet — ESP-IDF
# builds components in a sub-scope and doesn't promote a component ALL custom target
# into the default build, so `version` still reports build 0/unknown on esp32. TODO:
# add it at the project level with explicit ordering before the main component.
