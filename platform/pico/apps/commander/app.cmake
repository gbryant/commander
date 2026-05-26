# Commander app: full module set, OTA, UART + telnet transports.
set(APP_USE_PFB ON)

# App-specific sources only — framework sources come from library targets below.
list(APPEND APP_SOURCES
    ${PICO_PLATFORM_DIR}/BootselModule.cpp
)

list(APPEND APP_PIO
    ${PICO_PLATFORM_DIR}/ir_rx.pio
)

list(APPEND APP_LINK_LIBS
    commander::core
    commander::hal_pico
    commander::transport_uart
    commander::transport_telnet
    commander::modules
    pico_lwip_mdns
    hardware_pio
    hardware_clocks
    hardware_flash
    hardware_sync
    pico_multicore
    pico_fota_bootloader_lib
)
