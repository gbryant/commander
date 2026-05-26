set(APP_USE_PFB OFF)

list(APPEND APP_LINK_LIBS
    hardware_uart
    hardware_gpio
    hardware_watchdog
    hardware_exception
)
