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

// How many modules the UART task can pump via tick(). Each slot is one pointer.
// Raised from 2 when the breadboard-kit modules landed: a board with a display,
// touch, joystick, buttons, LEDs and a buzzer needs five pumped modules at once,
// and silently dropping the sixth is the same trap MAX_COMMANDS used to be.
// Override with -DCOMMANDER_MAX_TICKERS=<n>.
#ifndef COMMANDER_MAX_TICKERS
#  if defined(__AVR__)
#    define COMMANDER_MAX_TICKERS 4      // 2 KB SRAM — 8 bytes of pointers
#  else
#    define COMMANDER_MAX_TICKERS 8
#  endif
#endif

class UartTransport {
public:
    // Call with baud to initialize UART hardware (e.g. Uno).
    // Call without baud on platforms where the framework already initialized Serial.
    void begin(CommandRegistry &reg, uint32_t baud, const char *greeting = nullptr);
    void begin(CommandRegistry &reg, const char *greeting = nullptr);
    void addTicker(IModule &m);
    // Number of tickers that didn't fit COMMANDER_MAX_TICKERS. Non-zero means a
    // module's tick() is never called — it looks exactly like dead hardware, so
    // the task announces it at startup rather than leaving you to find it.
    uint8_t droppedTickers() const { return _tickDropped; }

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

    static constexpr uint8_t kMaxTickers = COMMANDER_MAX_TICKERS;
    IModule *_tickers[kMaxTickers] = {};
    uint8_t  _tickCount   = 0;
    uint8_t  _tickDropped = 0;
};
