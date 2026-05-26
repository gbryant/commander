set(APP_USE_PFB      OFF)
set(APP_USE_FREERTOS OFF)

# Default pins: Pico 2W UART1 GP4(TX)/GP5(RX). Override with -DUART_TX_PIN=20 etc.
if(NOT DEFINED UART_TX_PIN)
    set(UART_TX_PIN 4)
endif()
if(NOT DEFINED UART_RX_PIN)
    set(UART_RX_PIN 5)
endif()
list(APPEND APP_COMPILE_DEFS UART_TX_PIN=${UART_TX_PIN} UART_RX_PIN=${UART_RX_PIN})

list(APPEND APP_LINK_LIBS
    hardware_uart
    hardware_gpio
)
