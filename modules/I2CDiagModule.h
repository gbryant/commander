#pragma once
#include "core/IModule.h"
#include "core/CommandRegistry.h"
#include "hal/hal.h"
#include <stdlib.h>   // strtol
#include <string.h>   // strlen, strncmp

// I2C bus diagnostics, exposed as a single "i2c" command:
//   i2c scan                       — probe 0x08..0x77, list devices that ACK
//   i2c read  <addr> <reg> [len]   — set-register read (len 1..32, default 1)
//   i2c write <addr> <reg> [byte…] — write a register + optional data bytes
//
// Platform-independent (pure hal_i2c_*) and one command slot, so it costs almost
// nothing and is disabled by default — `cmdr module enable i2c` when you need it.
// Handy for bringing up the locomotion bridge: `i2c scan` finds it at 0x42,
// `i2c write 0x42 0x10 0 200 128 0` sends a raw CMD_LOCO_DRIVE, and
// `i2c read 0x42 0x12 12` dumps the sensor snapshot.
//
// addr/reg/byte arguments accept 0x.. hex or decimal (strtol base 0).
class I2CDiagModule : public IModule {
public:
    const char *name() const override { return "i2c"; }
    void        init() override {}
    void        registerCommands(CommandRegistry &reg) override;

private:
    static void i2cCmd(const char *args, Writer &out, void *ctx);
    static void scan(Writer &out);
    static void readCmd(const char *p, Writer &out);
    static void writeCmd(const char *p, Writer &out);
    static void usage(Writer &out);

    // No-printf helpers, matching the codebase's manual formatting.
    static void putHex8(Writer &out, uint8_t v) {
        static const char H[] = "0123456789abcdef";
        char s[3]; s[0] = H[(v >> 4) & 0xF]; s[1] = H[v & 0xF]; s[2] = '\0';
        out.write(s);
    }
    static void putUInt(Writer &out, uint32_t v) {
        char tmp[11]; int t = 0;
        if (v == 0) tmp[t++] = '0';
        while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
        char s[12]; int i = 0;
        while (t) s[i++] = tmp[--t];
        s[i] = '\0';
        out.write(s);
    }
    static const char *skipSpaces(const char *p) { while (*p == ' ') ++p; return p; }
    static const char *nextTok(const char *p) { while (*p && *p != ' ') ++p; return skipSpaces(p); }
    static bool tokIs(const char *p, const char *t) {
        size_t n = strlen(t);
        return strncmp(p, t, n) == 0 && (p[n] == '\0' || p[n] == ' ');
    }
};

inline void I2CDiagModule::usage(Writer &out) {
    out.writeln("i2c scan                       scan the bus (0x08..0x77)");
    out.writeln("i2c read  <addr> <reg> [len]   read len bytes (1..32, default 1) from a register");
    out.writeln("i2c write <addr> <reg> [byte…] write a register + optional data bytes");
    out.writeln("    addr/reg/bytes accept 0x.. hex or decimal");
}

inline void I2CDiagModule::scan(Writer &out) {
    out.writeln("scanning 0x08..0x77...");
    uint32_t found = 0;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        if (hal_i2c_probe(a)) {
            out.write("  device at 0x"); putHex8(out, a); out.writeln();
            found++;
        }
    }
    putUInt(out, found); out.writeln(" device(s) found");
}

inline void I2CDiagModule::readCmd(const char *p, Writer &out) {
    char *end;
    long addr = strtol(p, &end, 0);
    if (end == p) { usage(out); return; }
    p = end;
    long reg = strtol(p, &end, 0);
    if (end == p) { usage(out); return; }
    p = end;
    long len = strtol(p, &end, 0);
    if (end == p) len = 1;
    if (len < 1)  len = 1;
    if (len > 32) len = 32;

    uint8_t buf[32];
    if (!hal_i2c_read((uint8_t)addr, (uint8_t)reg, buf, (size_t)len)) {
        out.writeln("read failed (no ACK?)");
        return;
    }
    out.write("0x"); putHex8(out, (uint8_t)addr);
    out.write(" reg 0x"); putHex8(out, (uint8_t)reg); out.write(":");
    for (long i = 0; i < len; i++) { out.write(" "); putHex8(out, buf[i]); }
    out.writeln();
}

inline void I2CDiagModule::writeCmd(const char *p, Writer &out) {
    char *end;
    long addr = strtol(p, &end, 0);
    if (end == p) { usage(out); return; }
    p = end;
    long reg = strtol(p, &end, 0);
    if (end == p) { usage(out); return; }
    p = end;

    uint8_t data[32];
    size_t n = 0;
    while (n < sizeof(data)) {
        long v = strtol(p, &end, 0);
        if (end == p) break;       // no more numbers
        data[n++] = (uint8_t)v;
        p = end;
    }
    bool ok = hal_i2c_write((uint8_t)addr, (uint8_t)reg, n ? data : nullptr, n);
    if (!ok) { out.writeln("write failed (no ACK?)"); return; }
    out.write("ok: wrote "); putUInt(out, (uint32_t)n);
    out.write(" byte(s) to 0x"); putHex8(out, (uint8_t)addr);
    out.write(" reg 0x"); putHex8(out, (uint8_t)reg); out.writeln();
}

inline void I2CDiagModule::i2cCmd(const char *args, Writer &out, void *) {
    const char *p = skipSpaces(args);
    if (*p == '\0' || tokIs(p, "help")) { usage(out); return; }
    if (tokIs(p, "scan"))  { scan(out); return; }
    if (tokIs(p, "read"))  { readCmd(nextTok(p), out); return; }
    if (tokIs(p, "write")) { writeCmd(nextTok(p), out); return; }
    out.write("unknown i2c subcommand: "); out.writeln(p);
    usage(out);
}

inline void I2CDiagModule::registerCommands(CommandRegistry &reg) {
    reg.registerCommand(CMD("i2c", "I2C bus diagnostics - 'i2c' for usage", I2C_NONE, i2cCmd, nullptr));
}
