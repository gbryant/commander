#include "UartTransport.h"
#include "hal/hal.h"
#include <string.h>

namespace {
struct UartWriter : Writer {
    void write(const char *s) override { hal_uart_puts(s); }
    void writeln(const char *s) override { hal_uart_puts(s); hal_uart_puts("\r\n"); }
};
}

void UartTransport::begin(CommandRegistry &reg, uint32_t baud) {
    _reg = &reg;
    hal_uart_init(baud);
}

void UartTransport::prompt() {
    hal_uart_puts("> ");
}

void UartTransport::handleByte(char c) {
    if (c == '\n') return;

    if (c == '\r') {
        hal_uart_puts("\r\n");
        if (_pos > 0) {
            _buf[_pos] = '\0';
            UartWriter out;
            _reg->dispatch(_buf, out);
            _pos = 0;
        }
        prompt();
        return;
    }

    if (c == 0x7F || c == 0x08) {  // DEL / backspace
        if (_pos > 0) {
            _pos--;
            hal_uart_puts("\b \b");
        }
        return;
    }

    if (c < 0x20) return;  // ignore other control chars

    if (_pos < sizeof(_buf) - 1) {
        _buf[_pos++] = c;
        hal_uart_putchar(c);  // echo
    }
}

void UartTransport::taskBody(void *self) {
    auto *t = static_cast<UartTransport *>(self);
    t->prompt();
    for (;;) {
        int c = hal_uart_getchar(10);  // 10 ms poll
        if (c >= 0) t->handleByte((char)c);
    }
}
