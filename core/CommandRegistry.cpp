#include "CommandRegistry.h"
#include <string.h>
#include <stdio.h>

// Override in platform code to add board-specific diagnostic output (LED blink, etc.)
__attribute__((weak)) void commander_on_panic() { for (;;) {} }

void CommandRegistry::registerCommand(const Command &cmd) {
    if (_count >= kMaxCommands) {       // registry full — don't drop it silently
        _dropped++;
#ifndef LOW_MEMORY_MODE
        if (!_firstDropped) _firstDropped = cmd.name;
#endif
        return;
    }
    _commands[_count++] = cmd;
}

void CommandRegistry::registerModule(IModule &module) {
    module.init();
    module.registerCommands(*this);
}

void CommandRegistry::dispatch(const char *line, Writer &out) const {
#ifdef LOW_MEMORY_MODE
    uint8_t id = static_cast<uint8_t>(*line);
    for (size_t i = 0; i < _count; i++) {
        if (_commands[i].i2c_id == id) {
            _commands[i].handler(line + 1, out, _commands[i].ctx);
            return;
        }
    }
#else
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
#endif
}

void CommandRegistry::validateIds() const {
    if (_dropped) {                     // loud at boot on the serial log
#ifndef LOW_MEMORY_MODE
        printf("[WARN] %u command(s) dropped (e.g. '%s') — MAX_COMMANDS=%u too small\n",
               (unsigned)_dropped, _firstDropped ? _firstDropped : "?", (unsigned)kMaxCommands);
#else
        printf("[WARN] %u command(s) dropped — MAX_COMMANDS=%u too small\n",
               (unsigned)_dropped, (unsigned)kMaxCommands);
#endif
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

#ifndef LOW_MEMORY_MODE
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
#endif
