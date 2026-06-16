#pragma once
#include <WiFiS3.h>
#include "core/CommandRegistry.h"

// Matches COMMANDER_UART_LINE_BUF in UartTransport.h — override with -D to change both.
#ifndef COMMANDER_UART_LINE_BUF
#  define COMMANDER_UART_LINE_BUF 256
#endif

// Single-client telnet server using Arduino WiFiServer/WiFiClient (WiFiS3).
// Drop-in for TelnetTransport on Arduino WiFi platforms (e.g. Arduino R4 WiFi).
class ArduinoTelnetTransport {
public:
    void begin(CommandRegistry& reg, WiFiServer& server, const char* greeting = nullptr);
    static void taskBody(void* self);  // FreeRTOS task entry

    // Optional poll hook called from the task loop when idle or between client bytes.
    // Use this to run other WiFiS3-touching code (OTA, mDNS) from the same task,
    // since WiFiS3 has no internal locking.
    void setPollFn(void (*fn)()) { _pollFn = fn; }

private:
    void serveClient(WiFiClient client);
    void handleByte(char c, WiFiClient& client);
    void sendPrompt(WiFiClient& client);

    CommandRegistry* _reg      = nullptr;
    WiFiServer*      _server   = nullptr;
    const char*      _greeting = nullptr;
    void           (*_pollFn)() = nullptr;
    char     _buf[COMMANDER_UART_LINE_BUF] = {};
    uint16_t _pos     = 0;
    bool    _saw_iac  = false;
    bool    _skip_opt = false;
};
