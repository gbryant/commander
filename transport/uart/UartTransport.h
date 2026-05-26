#pragma once
#include "core/CommandRegistry.h"
#include "core/IModule.h"
#include "core/Writer.h"

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
    char    _buf[64];
    uint8_t _pos = 0;

    static constexpr uint8_t kMaxTickers = 2;
    IModule *_tickers[kMaxTickers] = {};
    uint8_t  _tickCount = 0;
};
