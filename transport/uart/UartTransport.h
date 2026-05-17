#pragma once
#include "core/CommandRegistry.h"
#include "core/Writer.h"

class UartTransport {
public:
    void begin(CommandRegistry &reg, uint32_t baud = 115200, const char *greeting = nullptr);

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
};
