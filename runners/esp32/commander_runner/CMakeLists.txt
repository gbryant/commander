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

idf_component_register(
    SRCS
        "runner.cpp"
        "${COMMANDER_ROOT}/core/CommandRegistry.cpp"
        "${COMMANDER_ROOT}/hal/esp32/hal.cpp"
        "${COMMANDER_ROOT}/transport/uart/UartTransport.cpp"
        "${COMMANDER_ROOT}/transport/telnet/TelnetTransport.cpp"
    INCLUDE_DIRS
        "${COMMANDER_ROOT}"
        "${COMMANDER_ROOT}/include"
    REQUIRES
        esp_driver_i2c
        esp_driver_gpio
        esp_driver_usb_serial_jtag
        esp_timer
        freertos
        nvs_flash
        esp_netif
        esp_wifi
        esp_event
        espressif__mdns
)
