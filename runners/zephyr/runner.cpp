// commander_runner — Zephyr.
//
// Owns main(), the CommandRegistry, and the transport thread; the consumer's app
// provides commander_config() + commander_setup(), and optional hooks get weak
// no-op defaults — same contract as the other runners (see commander.h).
//
// The console rides the board's chosen `zephyr,console` UART (hal/zephyr/hal.cpp).
// Route that to the right pins/peripheral with a devicetree overlay — on the Arduino
// Uno Q that's `lpuart1` (the QRB bridge UART -> /dev/ttyHS1), via:
//   chosen { zephyr,console = &lpuart1; };
//
// Transport is selectable at build time. Default = the UART line-editor console.
// Define COMMANDER_ENABLE_CHANNELS to instead run the multiplexed channel bus
// (transport/channels/ChannelBusRunner): ch0 = console, other channels = peer pub/sub
// for a host/SBC broker. The app wires publishers + module tickers in the
// commander_on_channel_bus_ready() hook. See docs/commander-channels-bringup.md.
//
// No WiFi/telnet here: on Linux-hosted boards (Uno Q) networking is the host's job,
// not the MCU's. commander_on_wifi_connected() is a weak no-op for source compat.
#include "commander.h"
#include "hal/hal.h"
#include <zephyr/kernel.h>

#ifdef COMMANDER_ENABLE_CHANNELS
#include "transport/channels/ChannelBusRunner.h"
#else
#include "transport/uart/UartTransport.h"
#endif

#ifndef COMMANDER_ZEPHYR_UART_STACK
#define COMMANDER_ZEPHYR_UART_STACK 4096
#endif

static CommanderConfig _cfg;
static CommandRegistry _registry;

extern "C" __attribute__((weak)) void commander_early_init()        {}
extern "C" __attribute__((weak)) void commander_on_wifi_connected() {}

#ifdef COMMANDER_ENABLE_CHANNELS
static ChannelBusRunner _bus;
// Called after commander_setup(), before the bus thread starts. Wire channel publishers
// (bus.channels().publisher(ch)) and add module tickers (bus.addTicker(m)) here.
extern "C" __attribute__((weak)) void commander_on_channel_bus_ready(ChannelBusRunner &) {}
static void transport_entry(void *p1, void *, void *) { ChannelBusRunner::taskBody(p1); }
#else
static UartTransport _uart;
extern "C" __attribute__((weak)) void commander_on_uart_ready(UartTransport &) {}
static void transport_entry(void *p1, void *, void *) { UartTransport::taskBody(p1); }
#endif

K_THREAD_STACK_DEFINE(_cmdr_uart_stack, COMMANDER_ZEPHYR_UART_STACK);
static struct k_thread _cmdr_uart_thread;

int main(void) {
    commander_early_init();
    _cfg = commander_config();

    if (_cfg.i2c_sda >= 0)
        hal_i2c_init((uint8_t)_cfg.i2c_sda, (uint8_t)_cfg.i2c_scl, _cfg.i2c_hz);

#ifdef COMMANDER_ENABLE_CHANNELS
    _bus.begin(_registry, _cfg.uart_baud);          // brings up the HAL UART + channel mux
    commander_setup(_registry);
    commander_on_channel_bus_ready(_bus);
    void *param = &_bus;
#else
    _uart.begin(_registry, _cfg.uart_baud, _cfg.uart_greeting);  // brings up the HAL UART
    commander_setup(_registry);
    commander_on_uart_ready(_uart);
    void *param = &_uart;
#endif

    k_thread_create(&_cmdr_uart_thread, _cmdr_uart_stack,
                    K_THREAD_STACK_SIZEOF(_cmdr_uart_stack),
                    transport_entry, param, nullptr, nullptr,
                    5 /*prio*/, 0, K_NO_WAIT);
    return 0;   // the kernel keeps the transport thread running
}
