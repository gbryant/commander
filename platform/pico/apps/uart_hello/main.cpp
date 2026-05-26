#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include <cstdio>

int main() {
    stdio_init_all();
    sleep_ms(2000);

    uart_init(uart1, 115200);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    uart_set_format(uart1, 8, 1, UART_PARITY_NONE);

    // Echo UART1 RX → USB CDC. Makes Pico W a transparent bridge for
    // the other board's UART debug output.
    for (;;) {
        while (uart_is_readable(uart1))
            putchar_raw(uart_getc(uart1));
        sleep_ms(1);
    }
}
