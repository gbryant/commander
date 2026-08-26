#pragma once
#include "core/IModule.h"
#include "core/CommandRegistry.h"
#include "core/CmdArgs.h"
#include "core/NullWriter.h"
#include "hal/hal.h"
#include "modules/ConsoleOut.h"
#include <stdint.h>
#include <string.h>

// ── Momentary push buttons on GPIO ───────────────────────────────────────────
// Portable (hal_gpio_* only) and multi-instance: one module handles up to
// kMaxButtons wired anywhere, exposed as one namespaced `btn` command.
//
// Debounced by *time*, in tick(), not by interrupts. The original breadboard-kit
// code used edge IRQs with a 2 ms window, which is the classic way to get phantom
// presses — a bouncing contact fires the ISR faster than the guard, and printing
// from inside the ISR makes it worse. Polling at tick rate with a settle window
// costs nothing here (the UART task is already running) and cannot re-enter.
//
// Publishes three ways, like the joystick: poll (pressed()), push (onPress
// callback), and declarative `btn bind <n> <command…>`.

class ButtonsModule : public IModule {
public:
    static constexpr int kMaxButtons = 8;

    // pins: array of GPIO numbers. activeLow suits the usual pull-up wiring
    // (button shorts to ground when pressed).
    ButtonsModule(const uint8_t *pins, uint8_t count, bool activeLow = true,
                  uint16_t debounceMs = 25)
        : _count(count > kMaxButtons ? kMaxButtons : count),
          _activeLow(activeLow), _debounceMs(debounceMs) {
        for (int i = 0; i < _count; i++) _pin[i] = pins[i];
    }

    const char *name() const override { return "btn"; }

    void init() override {
        for (int i = 0; i < _count; i++) {
            hal_gpio_set_input(_pin[i]);
            _stable[i] = _raw[i] = readPin(i);
            _changedAt[i] = 0;
        }
    }

    void registerCommands(CommandRegistry &reg) override;

    void tick() override {
        uint64_t now = hal_time_us();
        for (int i = 0; i < _count; i++) {
            bool level = readPin(i);
            if (level != _raw[i]) {            // bouncing: restart the settle window
                _raw[i] = level;
                _changedAt[i] = now;
                continue;
            }
            if (level == _stable[i]) continue;
            if (now - _changedAt[i] < (uint64_t)_debounceMs * 1000) continue;
            _stable[i] = level;                // settled → a real edge
            _count_[i] += level ? 1 : 0;
            if (_cb) _cb((uint8_t)i, level, _cbCtx);
            if (level) runBinding(i);
            if (_streaming) {
                // Console, not the Writer that started the stream — see
                // modules/ConsoleOut.h.
                console::puts("btn ");
                console::putUInt((uint32_t)i);
                console::puts(level ? " down" : " up");
                console::endl();
            }
        }
    }

    // ── App API ──────────────────────────────────────────────────────────────
    uint8_t count() const { return _count; }
    bool pressed(int i) const { return i >= 0 && i < _count && _stable[i]; }
    uint32_t presses(int i) const { return (i >= 0 && i < _count) ? _count_[i] : 0; }

    using Callback = void (*)(uint8_t index, bool down, void *ctx);
    void onPress(Callback cb, void *ctx = nullptr) { _cb = cb; _cbCtx = ctx; }

private:
    static constexpr int kBindLen = 40;

    uint8_t  _pin[kMaxButtons]  = {};
    bool     _raw[kMaxButtons]  = {};
    bool     _stable[kMaxButtons] = {};
    uint64_t _changedAt[kMaxButtons] = {};
    uint32_t _count_[kMaxButtons] = {};
    char     _bind[kMaxButtons][kBindLen] = {};

    int      _count;
    bool     _activeLow;
    uint16_t _debounceMs;

    Callback _cb = nullptr; void *_cbCtx = nullptr;
    CommandRegistry *_reg = nullptr;
    bool    _streaming = false;

    // Returns logical "pressed", already corrected for wiring polarity.
    bool readPin(int i) const {
        bool level = hal_gpio_read(_pin[i]);
        return _activeLow ? !level : level;
    }

    void runBinding(int i) {
        if (!_reg || !_bind[i][0]) return;
        NullWriter sink;
        _reg->dispatch(_bind[i], sink);
    }

    static void btnCmd(const char *args, Writer &out, void *ctx);
    void        dispatch(const char *args, Writer &out);
    void        usage(Writer &out);
};

// Weak app hook — the generated commander_modules.h null-checks and calls this.
extern "C" void commander_on_buttons_ready(ButtonsModule &) __attribute__((weak));

// ─────────────────────────────────────────────────────────────────────────────

inline void ButtonsModule::usage(Writer &out) {
    out.writeln("btn                       state of every button");
    out.writeln("btn watch | stop          stream press/release events to the console");
    out.writeln("btn bind <n> <command…>   run a command when button n is pressed");
    out.writeln("btn unbind <n> | btn binds");
}

inline void ButtonsModule::dispatch(const char *args, Writer &out) {
    const char *p = cmdarg::skipSpaces(args);

    if (cmdarg::is(p, "help")) { usage(out); return; }

    if (cmdarg::is(p, "watch")) {
        _streaming = true;
        out.writeln("streaming button events to the board console — 'btn stop' to end");
        return;
    }
    if (cmdarg::is(p, "stop")) { _streaming = false; out.writeln("stopped"); return; }

    if (cmdarg::is(p, "bind")) {
        long n;
        const char *q = cmdarg::next(p);
        if (!cmdarg::integer(q, n, &q) || n < 0 || n >= _count) { usage(out); return; }
        if (cmdarg::empty(q)) { usage(out); return; }
        strncpy(_bind[n], q, kBindLen - 1);
        _bind[n][kBindLen - 1] = '\0';
        out.write("bound button "); cmdarg::putUInt(out, (uint32_t)n);
        out.write(" -> "); out.writeln(_bind[n]);
        return;
    }
    if (cmdarg::is(p, "unbind")) {
        long n;
        if (!cmdarg::integer(cmdarg::next(p), n) || n < 0 || n >= _count) { usage(out); return; }
        _bind[n][0] = '\0';
        out.writeln("unbound");
        return;
    }
    if (cmdarg::is(p, "binds")) {
        bool any = false;
        for (int i = 0; i < _count; i++) {
            if (!_bind[i][0]) continue;
            any = true;
            out.write("  "); cmdarg::putUInt(out, (uint32_t)i);
            out.write(" -> "); out.writeln(_bind[i]);
        }
        if (!any) out.writeln("no bindings");
        return;
    }

    if (!cmdarg::empty(p)) { usage(out); return; }

    for (int i = 0; i < _count; i++) {
        out.write("  btn "); cmdarg::putUInt(out, (uint32_t)i);
        out.write(" (gp");   cmdarg::putUInt(out, _pin[i]);
        out.write("): ");    out.write(_stable[i] ? "down" : "up");
        out.write("   presses="); cmdarg::putUInt(out, _count_[i]);
        if (_bind[i][0]) { out.write("   -> "); out.write(_bind[i]); }
        out.writeln();
    }
    if (_count == 0) out.writeln("no buttons configured");
}

inline void ButtonsModule::btnCmd(const char *args, Writer &out, void *ctx) {
    static_cast<ButtonsModule *>(ctx)->dispatch(args, out);
}

inline void ButtonsModule::registerCommands(CommandRegistry &reg) {
    _reg = &reg;
    reg.registerCommand(CMD(
        "btn", "push buttons: state, watch, bind", I2C_NONE, btnCmd, this));
}
