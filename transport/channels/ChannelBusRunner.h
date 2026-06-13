#pragma once
#include "core/CommandRegistry.h"
#include "core/IModule.h"
#include "transport/channels/ChannelTransport.h"

// Runs the channel bus over the HAL UART — the bus-build counterpart to UartTransport
// (see docs/commander-channels-design.md). Same shape, so a platform main swaps one for
// the other: default-construct, begin(reg, baud), addTicker(module for async pumping),
// then hand the instance to the platform task creator (xTaskCreate / k_thread_create)
// with taskBody.
//
// Difference from UartTransport: there is NO local line editor / echo / prompt. The wire
// is message-oriented COBS frames, not a human terminal — ch0 carries a whole command
// line from the host broker (which owns echo/editing), commander dispatches it and frames
// the output back on ch0; other channels carry pub/sub. WriteFn loops hal_uart_putchar
// (NOT hal_uart_puts — frames contain 0x00).
//
// Optional/gated: only bus builds compile this; the AVR/MCU-console tier keeps
// UartTransport and pays nothing.
class ChannelBusRunner {
public:
    // baud overload inits the UART (e.g. bare-metal); no-baud assumes the framework /
    // runner already opened it. Both call the weak commander_on_channels_ready() hook so
    // the app can wire publishers/subscribers onto the freshly-begun transport.
    void begin(CommandRegistry &reg, uint32_t baud);
    void begin(CommandRegistry &reg);

    void addTicker(IModule &m);

    // The bus, for direct app use (the hook receives this same reference).
    ChannelTransport &channels() { return _ct; }

    // Pass this instance as the platform task parameter; loops RX -> feedByte + tickers.
    static void taskBody(void *self);

private:
    void wire(CommandRegistry &reg);   // shared begin() tail

    ChannelTransport _ct;

    static constexpr uint8_t kMaxTickers = 4;
    IModule *_tickers[kMaxTickers] = {};
    uint8_t  _tickCount = 0;
};
