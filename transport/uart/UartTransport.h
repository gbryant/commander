#pragma once
#include "core/CommandRegistry.h"
#include "core/IModule.h"
#include "core/Writer.h"

// Input line-editor buffer (one command line). Small by default on AVR (the Uno
// has 2 KB SRAM), roomier elsewhere so long arguments — e.g. a `read`/`marquee`
// message — aren't truncated. Override with -DCOMMANDER_UART_LINE_BUF=<n>.
#ifndef COMMANDER_UART_LINE_BUF
#  if defined(__AVR__)
#    define COMMANDER_UART_LINE_BUF 64
#  else
#    define COMMANDER_UART_LINE_BUF 256
#  endif
#endif

class UartTransport {
public:
    // Call with baud to initialize UART hardware (e.g. Uno).
    // Call without baud on platforms where the framework already initialized Serial.
    void begin(CommandRegistry &reg, uint32_t baud, const char *greeting = nullptr);
    void begin(CommandRegistry &reg, const char *greeting = nullptr);
    void addTicker(IModule &m);

    // Pass this instance as the FreeRTOS task parameter.
    // Platform main controls stack size and allocation strategy.
    static void taskBody(void *self);

private:
    void handleByte(char c);
    void prompt();

    CommandRegistry *_reg      = nullptr;
    const char      *_greeting = nullptr;
    char     _buf[COMMANDER_UART_LINE_BUF];
    uint16_t _pos = 0;                     // wide enough for buffers > 255

    static constexpr uint8_t kMaxTickers = 2;
    IModule *_tickers[kMaxTickers] = {};
    uint8_t  _tickCount = 0;
};
