#pragma once
#include "hal/hal.h"
#include <stdint.h>

// ── Writing from tick(), safely ──────────────────────────────────────────────
// A `Writer` is a STACK LOCAL owned by the transport for the duration of one
// dispatch() — `UartWriter out;` in UartTransport, `TelnetWriter out(fd);` in
// TelnetTransport. So a module that stashes `Writer*` during a command and uses
// it later from tick() is writing through a dangling pointer. It appears to work
// for as long as that stack is untouched, which is the worst way for it to fail.
//
// Tick-driven event streams therefore write to the board console directly, the
// way `ir recv` does. The trade is real and worth stating: **these streams go to
// the serial console, not to the telnet session that started them.** Anything
// that must reach the caller belongs in the command's own output, where the
// Writer is alive — or in a blocking loop inside the handler
// (`SerialMonitorModule::stream`), which keeps the Writer on the stack while it
// runs but holds the shell for the duration.
namespace console {

inline void puts(const char *s) { hal_uart_puts(s); }

inline void putUInt(uint32_t v) {
    char tmp[11]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    char s[12]; int i = 0;
    while (n) s[i++] = tmp[--n];
    s[i] = '\0';
    hal_uart_puts(s);
}

inline void putInt(int32_t v) {
    if (v < 0) { hal_uart_puts("-"); putUInt((uint32_t)(-(int64_t)v)); return; }
    putUInt((uint32_t)v);
}

inline void endl() { hal_uart_puts("\r\n"); }

}  // namespace console
