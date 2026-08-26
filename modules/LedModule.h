#pragma once
#include "core/IModule.h"
#include "core/CommandRegistry.h"
#include "core/CmdArgs.h"
#include "hal/hal.h"
#include <stdint.h>

// ── Plain indicator LEDs on GPIO ─────────────────────────────────────────────
// Portable (hal_gpio_* only). Boring on purpose: an app that wants one LED to
// mean something ("wifi up", "recording") shouldn't have to write pin plumbing,
// and a board's LEDs are the cheapest debugging instrument there is when the
// console is the thing that's broken.
//
// Blinking is driven from tick() rather than a task or a delay, so `led 0 blink
// 250` never blocks the shell.

class LedModule : public IModule {
public:
    static constexpr int kMaxLeds = 8;

    LedModule(const uint8_t *pins, uint8_t count, bool activeHigh = true)
        : _count(count > kMaxLeds ? kMaxLeds : count), _activeHigh(activeHigh) {
        for (int i = 0; i < _count; i++) _pin[i] = pins[i];
    }

    const char *name() const override { return "led"; }

    void init() override {
        for (int i = 0; i < _count; i++) {
            hal_gpio_set_output(_pin[i]);
            apply(i, false);
        }
    }

    void registerCommands(CommandRegistry &reg) override;

    void tick() override {
        uint64_t now = hal_time_us();
        for (int i = 0; i < _count; i++) {
            if (!_blinkMs[i]) continue;
            if (now - _lastToggle[i] < (uint64_t)_blinkMs[i] * 1000) continue;
            _lastToggle[i] = now;
            apply(i, !_on[i]);
        }
    }

    // ── App API ──────────────────────────────────────────────────────────────
    uint8_t count() const { return _count; }
    bool state(int i) const { return i >= 0 && i < _count && _on[i]; }
    void set(int i, bool on)  { if (valid(i)) { _blinkMs[i] = 0; apply(i, on); } }
    void toggle(int i)        { if (valid(i)) { _blinkMs[i] = 0; apply(i, !_on[i]); } }
    void all(bool on)         { for (int i = 0; i < _count; i++) set(i, on); }
    void blink(int i, uint16_t periodMs) {
        if (!valid(i)) return;
        _blinkMs[i] = periodMs;
        _lastToggle[i] = hal_time_us();
        if (periodMs) apply(i, true);
    }

private:
    uint8_t  _pin[kMaxLeds] = {};
    bool     _on[kMaxLeds]  = {};
    uint16_t _blinkMs[kMaxLeds] = {};
    uint64_t _lastToggle[kMaxLeds] = {};
    int      _count;
    bool     _activeHigh;

    bool valid(int i) const { return i >= 0 && i < _count; }
    void apply(int i, bool on) {
        _on[i] = on;
        hal_gpio_write(_pin[i], _activeHigh ? on : !on);
    }

    static void ledCmd(const char *args, Writer &out, void *ctx);
    void        dispatch(const char *args, Writer &out);
    void        usage(Writer &out);
};

// Weak app hook — the generated commander_modules.h null-checks and calls this.
extern "C" void commander_on_leds_ready(LedModule &) __attribute__((weak));

// ─────────────────────────────────────────────────────────────────────────────

inline void LedModule::usage(Writer &out) {
    out.writeln("led                       state of every LED");
    out.writeln("led <n> on|off|toggle");
    out.writeln("led <n> blink <ms>        0 ms stops blinking");
    out.writeln("led all on|off");
}

inline void LedModule::dispatch(const char *args, Writer &out) {
    const char *p = cmdarg::skipSpaces(args);

    if (cmdarg::is(p, "help")) { usage(out); return; }

    if (cmdarg::is(p, "all")) {
        bool on;
        if (!cmdarg::boolean(cmdarg::next(p), on)) { usage(out); return; }
        all(on);
        out.writeln(on ? "all on" : "all off");
        return;
    }

    if (cmdarg::empty(p)) {
        for (int i = 0; i < _count; i++) {
            out.write("  led "); cmdarg::putUInt(out, (uint32_t)i);
            out.write(" (gp");   cmdarg::putUInt(out, _pin[i]);
            out.write("): ");    out.write(_on[i] ? "on" : "off");
            if (_blinkMs[i]) {
                out.write("   blinking ");
                cmdarg::putUInt(out, _blinkMs[i]);
                out.write("ms");
            }
            out.writeln();
        }
        if (_count == 0) out.writeln("no LEDs configured");
        return;
    }

    long n;
    const char *q = p;
    if (!cmdarg::integer(q, n, &q) || !valid((int)n)) { usage(out); return; }

    if (cmdarg::is(q, "toggle")) { toggle((int)n); }
    else if (cmdarg::is(q, "blink")) {
        long ms;
        if (!cmdarg::integer(cmdarg::next(q), ms, 0, 60000)) { usage(out); return; }
        blink((int)n, (uint16_t)ms);
    } else {
        bool on;
        if (!cmdarg::boolean(q, on)) { usage(out); return; }
        set((int)n, on);
    }

    out.write("led "); cmdarg::putUInt(out, (uint32_t)n);
    out.write(": ");   out.write(_on[n] ? "on" : "off");
    if (_blinkMs[n]) { out.write(" (blinking "); cmdarg::putUInt(out, _blinkMs[n]); out.write("ms)"); }
    out.writeln();
}

inline void LedModule::ledCmd(const char *args, Writer &out, void *ctx) {
    static_cast<LedModule *>(ctx)->dispatch(args, out);
}

inline void LedModule::registerCommands(CommandRegistry &reg) {
    reg.registerCommand(CMD(
        "led", "indicator LEDs: on/off/toggle/blink", I2C_NONE, ledCmd, this));
}
