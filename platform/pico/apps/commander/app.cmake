# Commander app: full module set, OTA, UART + WiFi/telnet transports.
# Framework boilerplate (main, WiFi, FreeRTOS wiring) comes from commander::pico_runner.
set(APP_USE_PFB ON)

list(APPEND APP_PIO
    ${PICO_PLATFORM_DIR}/ir_rx.pio
)

list(APPEND APP_LINK_LIBS
    commander::pico_runner
    hardware_pio
    hardware_clocks
    pico_multicore
    pico_fota_bootloader_lib
)
