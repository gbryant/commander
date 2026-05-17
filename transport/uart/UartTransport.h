#pragma once
#include "core/CommandRegistry.h"
#include "core/Writer.h"

class UartTransport {
public:
    void begin(CommandRegistry &reg, uint32_t baud = 115200);

    // Pass this instance as the FreeRTOS task parameter.
    // Platform main controls stack size and allocation strategy.
    static void taskBody(void *self);

private:
    void handleByte(char c);
    void prompt();

    CommandRegistry *_reg = nullptr;
    char    _buf[64];
    uint8_t _pos = 0;
};
