#pragma once
#include "core/CommandRegistry.h"
#include "core/Writer.h"

// Single-client telnet server over lwIP BSD sockets (port 23).
// Works on any platform with LWIP_SOCKET=1 (Pico W lwip_freertos, ESP-IDF).
class TelnetTransport {
public:
    void begin(CommandRegistry &reg, const char *greeting = nullptr);
    static void taskBody(void *self);  // FreeRTOS task entry

private:
    void serveClient(int fd);
    void handleByte(int fd, char c);
    void prompt(int fd);

    CommandRegistry *_reg      = nullptr;
    const char      *_greeting = nullptr;
    char    _buf[64]  = {};
    uint8_t _pos      = 0;
    bool    _saw_iac  = false;  // inside IAC telnet negotiation sequence
    bool    _skip_opt = false;  // waiting to skip WILL/WONT/DO/DONT option byte
};
