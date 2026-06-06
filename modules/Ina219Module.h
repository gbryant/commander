#pragma once
#include <stdint.h>
#include <string.h>   // strlen, strncmp
#include "hal/hal.h"
#include "core/IModule.h"
#include "core/CommandRegistry.h"
#include "i2c_ids.h"

// Texas Instruments INA219 current/power monitor(s) via I²C, exposed as a single
// namespaced "ina" command (one command slot, however many sensors you wire):
//   ina                      — list channels, each with a voltage/current summary
//   ina <ch>                 — bus voltage / current / power for one channel
//   ina <ch> volt|amp|watt   — a single reading
//   ina <ch> stats           — "mv,ma" CSV (one channel; what the solar logger polls)
//   ina stats                — "<ch>,mv,ma" CSV, one line per channel
//   ina <ch> init / ina init — re-run calibration (one channel / all)
//
// A "channel" is a labelled INA219 at a 7-bit address: addChannel("a", 0x40).
// Calibrated for a 0.1 Ω shunt (Adafruit / SparkFun standard breakout):
//   Current_LSB = 100 µA  → current register × 0.1 = mA
//   Power_LSB   = 2 mW    → power   register × 2   = mW
class Ina219Module : public IModule {
public:
    static constexpr uint8_t kMaxChannels = 4;

    Ina219Module() = default;

    // Add a sensor. `prefix` is a short label ("a", "b"); `addr` its 7-bit I²C
    // address. Returns false if full or the label is too long. Constructed from
    // cmdr-generated code (one addChannel per `channels` entry).
    bool addChannel(const char *prefix, uint8_t addr) {
        if (_n >= kMaxChannels) return false;
        size_t len = strlen(prefix);
        if (len == 0 || len >= sizeof(_ch[0].prefix)) return false;
        memcpy(_ch[_n].prefix, prefix, len + 1);
        _ch[_n].addr = addr;
        _n++;
        return true;
    }

    const char *name() const override { return "ina"; }
    void        init()        override { for (uint8_t i = 0; i < _n; i++) initChannel(_ch[i].addr); }
    void        registerCommands(CommandRegistry &reg) override;
    void        startTask()   override {}

private:
    struct Channel { char prefix[8]; uint8_t addr; };
    Channel _ch[kMaxChannels];
    uint8_t _n = 0;

    // ---- INA219 register access ------------------------------------------
    static bool readReg(uint8_t addr, uint8_t reg, uint16_t &out) {
        uint8_t buf[2];
        if (!hal_i2c_read(addr, reg, buf, 2)) return false;
        out = (uint16_t)((uint16_t)buf[0] << 8 | buf[1]);
        return true;
    }

    static void initChannel(uint8_t addr) {
        // INA219 needs ~1 ms after Vcc stable before it ACKs writes; without this
        // the cal write is silently lost and current/power read back stuck at 0.
        hal_delay_ms(10);
        // 32 V bus range, ÷8 PGA (±320 mV shunt), 12-bit ADC, continuous.
        uint8_t cfg[2] = {0x39, 0x9F};
        hal_i2c_write(addr, 0x00, cfg, 2);
        // Cal = 4096 (0x1000) for 0.1 Ω shunt → Current_LSB = 100 µA.
        uint8_t cal[2] = {0x10, 0x00};
        hal_i2c_write(addr, 0x05, cal, 2);
    }

    // ---- no-printf number formatting (matches the codebase idiom) ---------
    static void putUInt(Writer &out, uint32_t v) {
        char tmp[11]; int t = 0;
        if (v == 0) tmp[t++] = '0';
        while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
        char s[12]; int i = 0;
        while (t) s[i++] = tmp[--t];
        s[i] = '\0';
        out.write(s);
    }
    // current register is signed, 1 LSB = 0.1 mA → "1044.3"
    static void putMilliAmps(Writer &out, int16_t cur) {
        if (cur < 0) {
            out.write("-");
            cur = (cur == (int16_t)-32768) ? (int16_t)32767 : (int16_t)-cur;
        }
        uint16_t u = (uint16_t)cur;
        putUInt(out, (uint32_t)(u / 10));
        char f[3] = {'.', (char)('0' + (u % 10)), '\0'};
        out.write(f);
    }

    // ---- per-channel readers (true on I²C success) -----------------------
    static bool emitVolt(Writer &out, uint8_t addr) {
        uint16_t raw;
        if (!readReg(addr, 0x02, raw)) return false;
        putUInt(out, (uint32_t)(raw >> 3) * 4);   // bits [15:3], 1 LSB = 4 mV
        out.write(" mV");
        return true;
    }
    static bool emitAmp(Writer &out, uint8_t addr) {
        uint16_t raw;
        if (!readReg(addr, 0x04, raw)) return false;
        putMilliAmps(out, (int16_t)raw);
        out.write(" mA");
        return true;
    }
    static bool emitWatt(Writer &out, uint8_t addr) {
        uint16_t raw;
        if (!readReg(addr, 0x03, raw)) return false;
        putUInt(out, (uint32_t)raw * 2);          // 1 LSB = 2 mW
        out.write(" mW");
        return true;
    }
    // "mv,ma" from one atomic read pair (same ADC cycle, ~1 ms apart)
    static bool emitStatsCsv(Writer &out, uint8_t addr) {
        uint16_t vraw, iraw;
        if (!readReg(addr, 0x02, vraw) || !readReg(addr, 0x04, iraw)) return false;
        putUInt(out, (uint32_t)(vraw >> 3) * 4);
        out.write(",");
        putMilliAmps(out, (int16_t)iraw);
        return true;
    }

    // ---- dispatch --------------------------------------------------------
    const Channel *find(const char *tok) const {
        for (uint8_t i = 0; i < _n; i++) {
            size_t len = strlen(_ch[i].prefix);
            if (strncmp(tok, _ch[i].prefix, len) == 0 &&
                (tok[len] == '\0' || tok[len] == ' '))
                return &_ch[i];
        }
        return nullptr;
    }

    static void inaCmd(const char *args, Writer &out, void *ctx);
    void        dispatch(const char *args, Writer &out);
    void        usage(Writer &out);

    static const char *skipSpaces(const char *p) { while (*p == ' ') ++p; return p; }
    static const char *nextTok(const char *p) { while (*p && *p != ' ') ++p; return skipSpaces(p); }
    static bool tokIs(const char *p, const char *t) {
        size_t n = strlen(t);
        return strncmp(p, t, n) == 0 && (p[n] == '\0' || p[n] == ' ');
    }
};

inline void Ina219Module::usage(Writer &out) {
    out.writeln("ina                  list channels (label addr  v/a)");
    out.writeln("ina <ch>             voltage / current / power for one channel");
    out.writeln("ina <ch> volt|amp|watt|stats|init");
    out.writeln("ina stats            \"<ch>,mv,ma\" CSV, one line per channel");
    out.writeln("ina init             re-run calibration on all channels");
}

inline void Ina219Module::dispatch(const char *args, Writer &out) {
    const char *p = skipSpaces(args);

    // bare `ina` → list channels with a quick volt/amp summary
    if (*p == '\0') {
        if (_n == 0) { out.writeln("no INA219 channels"); return; }
        for (uint8_t i = 0; i < _n; i++) {
            out.write(_ch[i].prefix); out.write("  0x");
            char h[3]; static const char H[] = "0123456789abcdef";
            h[0] = H[(_ch[i].addr >> 4) & 0xF]; h[1] = H[_ch[i].addr & 0xF]; h[2] = '\0';
            out.write(h); out.write("  ");
            if (emitVolt(out, _ch[i].addr)) { out.write("  "); emitAmp(out, _ch[i].addr); }
            else out.write("err");
            out.writeln();
        }
        return;
    }
    if (tokIs(p, "help")) { usage(out); return; }

    // global (no channel) subcommands
    if (tokIs(p, "init")) { init(); out.writeln("ok"); return; }
    if (tokIs(p, "stats")) {
        for (uint8_t i = 0; i < _n; i++) {
            out.write(_ch[i].prefix); out.write(",");
            if (!emitStatsCsv(out, _ch[i].addr)) out.write("err");
            out.writeln();
        }
        return;
    }

    // otherwise the first token is a channel label
    const Channel *ch = find(p);
    if (!ch) { out.write("unknown channel: "); out.writeln(p); usage(out); return; }

    const char *sub = nextTok(p);
    bool ok = true;
    if (*sub == '\0') {                       // `ina <ch>` → full summary
        ok = emitVolt(out, ch->addr); if (ok) out.writeln();
        if (ok) { ok = emitAmp(out, ch->addr); if (ok) out.writeln(); }
        if (ok) { ok = emitWatt(out, ch->addr); if (ok) out.writeln(); }
    } else if (tokIs(sub, "volt"))  { ok = emitVolt(out, ch->addr); if (ok) out.writeln(); }
    else if (tokIs(sub, "amp"))     { ok = emitAmp(out, ch->addr);  if (ok) out.writeln(); }
    else if (tokIs(sub, "watt"))    { ok = emitWatt(out, ch->addr); if (ok) out.writeln(); }
    else if (tokIs(sub, "stats"))   { ok = emitStatsCsv(out, ch->addr); if (ok) out.writeln(); }
    else if (tokIs(sub, "init"))    { initChannel(ch->addr); out.writeln("ok"); return; }
    else { out.write("unknown: "); out.writeln(sub); usage(out); return; }

    if (!ok) out.writeln("err");
}

inline void Ina219Module::inaCmd(const char *args, Writer &out, void *ctx) {
    static_cast<Ina219Module *>(ctx)->dispatch(args, out);
}

inline void Ina219Module::registerCommands(CommandRegistry &reg) {
    // I2C_NONE: local sensor reads, not I²C relay commands. One slot, sub-dispatched.
    reg.registerCommand(CMD("ina", "INA219 current/power monitor - 'ina' to list", I2C_NONE, inaCmd, this));
}
