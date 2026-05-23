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

idf_component_register(
    SRCS
        "main.cpp"
        "../../../hal/esp32/hal.cpp"
        "../../../core/CommandRegistry.cpp"
        "../../../transport/uart/UartTransport.cpp"
        "../../../transport/telnet/TelnetTransport.cpp"
    INCLUDE_DIRS
        "../../.."
        "../../../include"
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
        app_update
        esp_http_client
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    BUILD_COMMIT="${GIT_COMMIT}"
    BUILD_NUMBER=${GIT_BUILD_NUMBER}
)
