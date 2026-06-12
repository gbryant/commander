// commander_runner — Zephyr.
//
// Owns main(), the CommandRegistry, and the UART transport thread; the consumer's
// app provides commander_config() + commander_setup(), and optional hooks get weak
// no-op defaults — same contract as the other runners (see commander.h).
//
// The console rides the board's chosen `zephyr,console` UART (hal/zephyr/hal.cpp).
// Route that to the right pins/peripheral with a devicetree overlay — on the Arduino
// Uno Q that's `lpuart1` (the QRB bridge UART -> /dev/ttyHS1), via:
//   chosen { zephyr,console = &lpuart1; };
//
// No WiFi/telnet here: on Linux-hosted boards (Uno Q) networking is the host's job,
// not the MCU's. commander_on_wifi_connected() is a weak no-op for source compat.
#include "commander.h"
#include "hal/hal.h"
#include "transport/uart/UartTransport.h"
#include <zephyr/kernel.h>

#ifndef COMMANDER_ZEPHYR_UART_STACK
#define COMMANDER_ZEPHYR_UART_STACK 4096
#endif

static CommanderConfig _cfg;
static CommandRegistry _registry;
static UartTransport   _uart;

extern "C" __attribute__((weak)) void commander_early_init()                   {}
extern "C" __attribute__((weak)) void commander_on_uart_ready(UartTransport &) {}
extern "C" __attribute__((weak)) void commander_on_wifi_connected()            {}

K_THREAD_STACK_DEFINE(_cmdr_uart_stack, COMMANDER_ZEPHYR_UART_STACK);
static struct k_thread _cmdr_uart_thread;

static void uart_entry(void *p1, void *, void *) { UartTransport::taskBody(p1); }

int main(void) {
    commander_early_init();
    _cfg = commander_config();

    if (_cfg.i2c_sda >= 0)
        hal_i2c_init((uint8_t)_cfg.i2c_sda, (uint8_t)_cfg.i2c_scl, _cfg.i2c_hz);

    _uart.begin(_registry, _cfg.uart_baud, _cfg.uart_greeting);  // brings up the HAL UART
    commander_setup(_registry);
    commander_on_uart_ready(_uart);

    k_thread_create(&_cmdr_uart_thread, _cmdr_uart_stack,
                    K_THREAD_STACK_SIZEOF(_cmdr_uart_stack),
                    uart_entry, &_uart, nullptr, nullptr,
                    5 /*prio*/, 0, K_NO_WAIT);
    return 0;   // the kernel keeps the uart thread running
}
