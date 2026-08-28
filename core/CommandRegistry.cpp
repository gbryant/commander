#include "CommandRegistry.h"
#include <string.h>
#include <stdio.h>

// Override in platform code to add board-specific diagnostic output (LED blink, etc.)
__attribute__((weak)) void commander_on_panic() { for (;;) {} }

// Weak default — no autostart commands. The generated commander_modules.h provides a
// strong override when `cmdr autostart` has configured any (see CommandRegistry.h).
extern "C" __attribute__((weak)) void commander_run_autostart(CommandRegistry &) {}

// Same for the ticker hook. Every runner also defines this weakly, which is
// harmless (the linker takes one no-op), but a COMMANDER_LIBRARIES_ONLY project
// has no runner — and without a default here it fails to link the moment it
// honours the runner contract and calls the hook. UartTransport is only
// forward-declared: core must not depend on a transport, and the parameter is
// unused. extern "C" keeps the symbol identical either way.
class UartTransport;
extern "C" __attribute__((weak)) void commander_on_uart_ready(UartTransport &) {}

void CommandRegistry::registerCommand(const Command &cmd) {
    if (_count >= kMaxCommands) {       // registry full — don't drop it silently
        _dropped++;
        if (!_firstDropped) _firstDropped = cmd.name;
        return;
    }
    _commands[_count++] = cmd;
}

void CommandRegistry::registerModule(IModule &module) {
    module.init();
    module.registerCommands(*this);
}

void CommandRegistry::dispatch(const char *line, Writer &out) const {
    while (*line == ' ') line++;
    if (*line == '\0') return;

    for (size_t i = 0; i < _count; i++) {
        const char *name = _commands[i].name;
        size_t      len  = strlen(name);
        if (strncmp(line, name, len) == 0 &&
                (line[len] == '\0' || line[len] == ' ')) {
            const char *args = (line[len] == ' ') ? line + len + 1 : "";
            _commands[i].handler(args, out, _commands[i].ctx);
            return;
        }
    }
    out.write("unknown: ");
    out.writeln(line);
}

void CommandRegistry::validateIds() const {
    if (_dropped) {                     // loud at boot on the serial log
        printf("[WARN] %u command(s) dropped (e.g. '%s') — MAX_COMMANDS=%u too small\n",
               (unsigned)_dropped, _firstDropped ? _firstDropped : "?", (unsigned)kMaxCommands);
    }
    for (size_t i = 0; i < _count; i++) {
        if (_commands[i].i2c_id == I2C_NONE) continue;
        for (size_t j = i + 1; j < _count; j++) {
            if (_commands[j].i2c_id == I2C_NONE) continue;
            if (_commands[i].i2c_id == _commands[j].i2c_id) {
                printf("[PANIC] duplicate command id: 0x%02X\n", _commands[i].i2c_id);
                commander_on_panic();
            }
        }
    }
}

void CommandRegistry::printHelp(Writer &out) const {
    for (size_t i = 0; i < _count; i++) {
        out.write("  ");
        out.write(_commands[i].name);
        out.write(" -- ");
        out.writeln(_commands[i].help);
    }
    if (_dropped) {                     // portable warning, visible over telnet/UART
        out.write("  !! commands dropped (e.g. '");
        out.write(_firstDropped ? _firstDropped : "?");
        out.writeln("') — MAX_COMMANDS too small; raise it and rebuild");
    }
}
