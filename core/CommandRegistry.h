#pragma once
#include <stdint.h>
#include <stddef.h>
#include "Writer.h"
#include "IModule.h"
#include "i2c_ids.h"

// Variadic-safe CMD macro — commas inside lambda bodies fool the preprocessor.
// Always declare multi-variable lines as separate statements inside handlers.
#ifdef LOW_MEMORY_MODE
#define CMD(name_str, help_str, id, fn, ctx_val)  { id, fn, ctx_val }
#else
#define CMD(name_str, help_str, id, fn, ctx_val)  { name_str, help_str, id, fn, ctx_val }
#endif

struct Command {
#ifndef LOW_MEMORY_MODE
    const char *name;
    const char *help;
#endif
    uint8_t     i2c_id;
    void      (*handler)(const char *args, Writer &out, void *ctx);
    void       *ctx;
};

class CommandRegistry {
public:
    void registerCommand(const Command &cmd);
    void registerModule(IModule &module);
    void dispatch(const char *input, Writer &out) const;
    void validateIds() const;

    // Number of commands that overflowed kMaxCommands and were dropped. Non-zero
    // means MAX_COMMANDS is too small — validateIds() warns at boot and printHelp()
    // flags it, so a too-small registry can't silently eat a command (it ate `ota`).
    size_t dropped() const { return _dropped; }

#ifndef LOW_MEMORY_MODE
    void printHelp(Writer &out) const;
#endif

private:
#ifdef MAX_COMMANDS
    static constexpr size_t kMaxCommands = MAX_COMMANDS;
#else
    static constexpr size_t kMaxCommands = 64;
#endif
    Command _commands[kMaxCommands];
    size_t  _count = 0;
    size_t  _dropped = 0;            // commands that didn't fit kMaxCommands
#ifndef LOW_MEMORY_MODE
    const char *_firstDropped = nullptr;
#endif
};
