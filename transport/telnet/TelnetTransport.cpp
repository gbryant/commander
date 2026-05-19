#include "TelnetTransport.h"
#include "lwip/sockets.h"
#include <string.h>

namespace {
struct TelnetWriter : Writer {
    int fd;
    explicit TelnetWriter(int f) : fd(f) {}
    void write(const char *s)   override { lwip_send(fd, s, strlen(s), 0); }
    void writeln(const char *s) override { write(s); write("\r\n"); }
};

void cmdDisconnect(const char *, Writer &out, void *ctx) {
    out.writeln("bye");
    *static_cast<bool *>(ctx) = true;
}
}

void TelnetTransport::begin(CommandRegistry &reg, const char *greeting) {
    _reg = &reg;
    _greeting = greeting;
    reg.registerCommand(CMD("disconnect", "close the telnet session",
        CMD_DISCONNECT, cmdDisconnect, &_disconnect));
}

void TelnetTransport::prompt(int fd) {
    lwip_send(fd, "> ", 2, 0);
}

void TelnetTransport::handleByte(int fd, char c) {
    // Strip RFC 854 IAC telnet negotiation sequences.
    // WILL/WONT/DO/DONT (0xFB-0xFE) are 3-byte sequences; others are 2-byte.
    if (_skip_opt) { _skip_opt = false; return; }
    if (_saw_iac) {
        _saw_iac = false;
        if ((uint8_t)c >= 0xFB) _skip_opt = true;
        return;
    }
    if ((uint8_t)c == 0xFF) { _saw_iac = true; return; }

    if (c == '\n') return;
    if (c == '\r') {
        lwip_send(fd, "\r\n", 2, 0);
        if (_pos > 0) {
            _buf[_pos] = '\0';
            TelnetWriter out(fd);
            _reg->dispatch(_buf, out);
            _pos = 0;
        }
        if (!_disconnect) prompt(fd);
        return;
    }
    if (c == 0x7F || c == 0x08) {
        if (_pos > 0) { _pos--; lwip_send(fd, "\b \b", 3, 0); }
        return;
    }
    if (c < 0x20) return;
    if (_pos < sizeof(_buf) - 1) {
        _buf[_pos++] = c;
        lwip_send(fd, &c, 1, 0);  // echo
    }
}

void TelnetTransport::serveClient(int fd) {
    _pos = 0; _saw_iac = false; _skip_opt = false; _disconnect = false;

    if (_greeting) {
        lwip_send(fd, _greeting, strlen(_greeting), 0);
        lwip_send(fd, "\r\n", 2, 0);
    }
    prompt(fd);

    // Plain blocking recv — in FreeRTOS this yields the CPU correctly.
    // SO_RCVTIMEO is avoided: with pico_cyw43_arch_lwip_sys_freertos the lock
    // interactions between the socket layer and async_context make it unreliable.
    char c;
    while (lwip_recv(fd, &c, 1, 0) == 1) {
        handleByte(fd, c);
        if (_disconnect) break;
    }
}

void TelnetTransport::taskBody(void *self) {
    auto *t = static_cast<TelnetTransport *>(self);

    int server_fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    lwip_setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(23);

    lwip_bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    lwip_listen(server_fd, 1);

    for (;;) {
        int client_fd = lwip_accept(server_fd, nullptr, nullptr);
        if (client_fd >= 0) {
            t->serveClient(client_fd);
            lwip_close(client_fd);
        }
    }
}
