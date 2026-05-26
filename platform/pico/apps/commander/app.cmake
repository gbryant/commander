# Commander app: full module set, OTA, UART + telnet transports.
set(APP_USE_PFB ON)

list(APPEND APP_SOURCES
    ${PROJECT_ROOT_DIR}/core/CommandRegistry.cpp
    ${PROJECT_ROOT_DIR}/hal/pico/hal.cpp
    ${PROJECT_ROOT_DIR}/transport/uart/UartTransport.cpp
    ${PROJECT_ROOT_DIR}/transport/telnet/TelnetTransport.cpp
    ${PICO_PLATFORM_DIR}/BootselModule.cpp
)

list(APPEND APP_PIO
    ${PICO_PLATFORM_DIR}/ir_rx.pio
)

list(APPEND APP_LINK_LIBS
    pico_cyw43_arch_lwip_sys_freertos
    pico_lwip_mdns
    hardware_i2c
    hardware_gpio
    hardware_pio
    hardware_clocks
    hardware_flash
    hardware_sync
    pico_multicore
    pico_fota_bootloader_lib
)
