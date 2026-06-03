#include "ArduinoTelnetTransport.h"
#include <Arduino_FreeRTOS.h>
#include <string.h>

namespace {
struct WiFiWriter : Writer {
    WiFiClient& client;
    explicit WiFiWriter(WiFiClient& c) : client(c) {}
    void write(const char* s)   override { client.print(s); }
    void writeln(const char* s) override { client.print(s); client.print("\r\n"); }
};
}

void ArduinoTelnetTransport::begin(CommandRegistry& reg, WiFiServer& server, const char* greeting) {
    _reg      = &reg;
    _server   = &server;
    _greeting = greeting;
    // No `disconnect` command — close the session from the client (telnet Ctrl-]
    // then `quit`, or just close the terminal).
}

void ArduinoTelnetTransport::sendPrompt(WiFiClient& client) {
    client.print("> ");
}

void ArduinoTelnetTransport::handleByte(char c, WiFiClient& client) {
    if (_skip_opt) { _skip_opt = false; return; }
    if (_saw_iac) {
        _saw_iac = false;
        if ((uint8_t)c >= 0xFB) _skip_opt = true;
        return;
    }
    if ((uint8_t)c == 0xFF) { _saw_iac = true; return; }

    if (c == '\n') return;
    if (c == '\r') {
        client.print("\r\n");
        if (_pos > 0) {
            _buf[_pos] = '\0';
            WiFiWriter out(client);
            _reg->dispatch(_buf, out);
            _pos = 0;
        }
        sendPrompt(client);
        return;
    }
    if (c == 0x7F || c == 0x08) {
        if (_pos > 0) { _pos--; client.print("\b \b"); }
        return;
    }
    if (c < 0x20) return;
    if (_pos < sizeof(_buf) - 1) {
        _buf[_pos++] = c;
        client.write((uint8_t)c);  // echo
    }
}

void ArduinoTelnetTransport::serveClient(WiFiClient client) {
    _pos = 0; _saw_iac = false; _skip_opt = false;

    if (_greeting) {
        client.print(_greeting);
        client.print("\r\n");
    }
    sendPrompt(client);

    while (client.connected()) {
        if (client.available()) {
            handleByte((char)client.read(), client);
        } else {
            if (_pollFn) _pollFn();
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    client.stop();
}

void ArduinoTelnetTransport::taskBody(void* self) {
    auto* t = static_cast<ArduinoTelnetTransport*>(self);
    for (;;) {
        WiFiClient client = t->_server->available();
        if (client) {
            t->serveClient(client);
        } else {
            if (t->_pollFn) t->_pollFn();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
