#pragma once
#include "core/IModule.h"
#include "core/CommandRegistry.h"
#include "core/CmdArgs.h"
#include "core/NullWriter.h"
#include "hal/hal.h"
#include "modules/ConsoleOut.h"
#include <stdint.h>
#include <string.h>

// ── Two-axis analog joystick (+ optional push switch) ────────────────────────
// Portable: hal_adc_* for the axes, hal_gpio_* for the button. Any platform
// whose HAL implements ADC gets it.
//
// Like the `controller` module, this is a generic *input source*, not tied to
// what it drives. It publishes three mix-and-match ways:
//   • poll    — x(), y(), direction(), pressed()
//   • push    — onDirection() / onButton() C++ callbacks
//   • declare — `joy bind up <command…>` dispatches any registered command
// so a project can wire a stick to a menu, a robot, or nothing at all without
// the module knowing which.
//
// Axes are published normalized to ±1000 around a centre measured at init, so
// consumers don't care about ADC width or a stick that doesn't rest at midscale.
// Calibration is spatial (re-centre + scale), never temporal — the same split
// ControllerCalibration/StickFilter draws, and for the same reason: a low-pass
// belongs at the consumer's fixed loop rate, not at the sample rate.

enum class JoyDir : uint8_t { Center = 0, Up, Down, Left, Right };

inline const char *joyDirName(JoyDir d) {
    switch (d) {
        case JoyDir::Up:    return "up";
        case JoyDir::Down:  return "down";
        case JoyDir::Left:  return "left";
        case JoyDir::Right: return "right";
        default:            return "center";
    }
}

class JoystickModule : public IModule {
public:
    // deadzonePct: how far the stick must move from centre (percent of full
    // travel) before a direction is reported. invertX/Y flip an axis whose
    // wiring runs the other way.
    JoystickModule(uint8_t xPin, uint8_t yPin, int8_t swPin = -1,
                   uint8_t deadzonePct = 30, bool invertX = false, bool invertY = false)
        : _xPin(xPin), _yPin(yPin), _sw(swPin),
          _deadzone(deadzonePct), _invX(invertX), _invY(invertY) {}

    const char *name() const override { return "joy"; }

    void init() override {
        _xCh = hal_adc_init(_xPin);
        _yCh = hal_adc_init(_yPin);
        _ok  = (_xCh >= 0 && _yCh >= 0);
        if (_sw >= 0) hal_gpio_set_input((uint8_t)_sw);
        if (_ok) calibrate();
    }

    void registerCommands(CommandRegistry &reg) override;

    void tick() override {
        if (!_ok) return;
        uint64_t now = hal_time_us();
        if (now - _lastPoll < kPollUs) return;
        _lastPoll = now;
        sample();

        JoyDir d = direction();
        if (d != _lastDir) {
            _lastDir = d;
            if (_dirCb) _dirCb(d, _dirCtx);
            runBinding(d);
            if (_streaming) {
                // Console, not the Writer that started the stream — see
                // modules/ConsoleOut.h.
                console::puts("joy ");
                console::puts(joyDirName(d));
                console::puts("  x="); console::putInt(_x);
                console::puts(" y=");  console::putInt(_y);
                console::endl();
            }
        }
        if (_sw >= 0) {
            bool p = pressed();
            if (p != _lastPressed) {
                _lastPressed = p;
                if (_btnCb) _btnCb(p, _btnCtx);
            }
        }
    }

    // ── App API ──────────────────────────────────────────────────────────────
    bool ready() const { return _ok; }
    int16_t x() const { return _x; }          // -1000..1000
    int16_t y() const { return _y; }
    uint16_t rawX() const { return _rawX; }
    uint16_t rawY() const { return _rawY; }
    bool pressed() const {
        return _sw >= 0 && !hal_gpio_read((uint8_t)_sw);   // switches are active-low
    }

    JoyDir direction() const {
        int16_t dz = (int16_t)(_deadzone * 10);            // percent → ±1000 scale
        // A stick pushed diagonally reports whichever axis is further out, so a
        // menu gets one unambiguous direction rather than two competing ones.
        int16_t ax = _x < 0 ? (int16_t)-_x : _x;
        int16_t ay = _y < 0 ? (int16_t)-_y : _y;
        if (ax < dz && ay < dz) return JoyDir::Center;
        if (ax >= ay) return _x > 0 ? JoyDir::Right : JoyDir::Left;
        return _y > 0 ? JoyDir::Up : JoyDir::Down;
    }

    // Re-measure the resting position. Call with the stick centred.
    void calibrate() {
        _centerX = average(_xCh);
        _centerY = average(_yCh);
        sample();
    }

    using DirCallback = void (*)(JoyDir, void *);
    using BtnCallback = void (*)(bool, void *);
    void onDirection(DirCallback cb, void *ctx = nullptr) { _dirCb = cb; _dirCtx = ctx; }
    void onButton(BtnCallback cb, void *ctx = nullptr)    { _btnCb = cb; _btnCtx = ctx; }

    void setDeadzone(uint8_t pct) { _deadzone = pct > 90 ? 90 : pct; }

private:
    static constexpr uint64_t kPollUs   = 20000;   // 50 Hz
    static constexpr int      kSamples  = 16;      // averaged per read (ADC noise)
    static constexpr int      kBindMax  = 5;       // one per JoyDir
    static constexpr int      kBindLen  = 40;

    uint8_t  _xPin, _yPin;
    int8_t   _sw;
    uint8_t  _deadzone;
    bool     _invX, _invY;
    int8_t   _xCh = -1, _yCh = -1;
    bool     _ok  = false;
    uint16_t _centerX = 0, _centerY = 0;
    uint16_t _rawX = 0, _rawY = 0;
    int16_t  _x = 0, _y = 0;
    JoyDir   _lastDir = JoyDir::Center;
    bool     _lastPressed = false;
    uint64_t _lastPoll = 0;

    DirCallback _dirCb = nullptr; void *_dirCtx = nullptr;
    BtnCallback _btnCb = nullptr; void *_btnCtx = nullptr;

    CommandRegistry *_reg = nullptr;
    char  _bind[kBindMax][kBindLen] = {};
    bool  _streaming = false;

    uint16_t average(int8_t ch) const {
        if (ch < 0) return 0;
        uint32_t total = 0;
        for (int i = 0; i < kSamples; i++) total += hal_adc_read((uint8_t)ch);
        return (uint16_t)(total / kSamples);
    }

    void sample() {
        _rawX = average(_xCh);
        _rawY = average(_yCh);
        _x = normalize(_rawX, _centerX, _invX);
        _y = normalize(_rawY, _centerY, _invY);
    }

    // Two-sided scaling: the distance from centre to each rail differs when the
    // stick doesn't rest at midscale, so each side gets its own divisor and full
    // deflection reaches ±1000 in both directions.
    static int16_t normalize(uint16_t raw, uint16_t center, bool invert) {
        int32_t full = hal_adc_max();
        if (full <= 0) return 0;
        int32_t v;
        if (raw >= center) {
            int32_t span = full - center;
            v = span > 0 ? ((int32_t)(raw - center) * 1000) / span : 0;
        } else {
            int32_t span = center;
            v = span > 0 ? -(((int32_t)(center - raw) * 1000) / span) : 0;
        }
        if (v > 1000)  v = 1000;
        if (v < -1000) v = -1000;
        return (int16_t)(invert ? -v : v);
    }

    void runBinding(JoyDir d) {
        if (!_reg) return;
        const char *cmd = _bind[(int)d];
        if (!cmd[0]) return;
        NullWriter sink;
        _reg->dispatch(cmd, sink);
    }

    static bool dirFromName(const char *p, JoyDir &out) {
        if (cmdarg::is(p, "up"))     { out = JoyDir::Up;     return true; }
        if (cmdarg::is(p, "down"))   { out = JoyDir::Down;   return true; }
        if (cmdarg::is(p, "left"))   { out = JoyDir::Left;   return true; }
        if (cmdarg::is(p, "right"))  { out = JoyDir::Right;  return true; }
        if (cmdarg::is(p, "center")) { out = JoyDir::Center; return true; }
        return false;
    }

    static void joyCmd(const char *args, Writer &out, void *ctx);
    void        dispatch(const char *args, Writer &out);
    void        usage(Writer &out);
};

// Weak app hook — the generated commander_modules.h null-checks and calls this.
extern "C" void commander_on_joystick_ready(JoystickModule &) __attribute__((weak));

// ─────────────────────────────────────────────────────────────────────────────

inline void JoystickModule::usage(Writer &out) {
    out.writeln("joy                        position, direction, button");
    out.writeln("joy raw                    raw ADC counts and the measured centre");
    out.writeln("joy cal                    re-measure centre (hold the stick centred)");
    out.writeln("joy deadzone <0-90>        percent of travel treated as centre");
    out.writeln("joy watch | stop           stream direction changes to the console");
    out.writeln("joy bind <dir> <command…>  run a command when the stick moves that way");
    out.writeln("joy unbind <dir> | joy binds");
    out.writeln("    <dir> = up | down | left | right | center");
}

inline void JoystickModule::dispatch(const char *args, Writer &out) {
    const char *p = cmdarg::skipSpaces(args);

    if (cmdarg::is(p, "help")) { usage(out); return; }

    if (!_ok && !cmdarg::is(p, "binds")) {
        out.writeln("joy: no ADC on the configured pins (check the HAL supports ADC)");
        return;
    }

    if (cmdarg::is(p, "raw")) {
        sample();
        out.write("raw x="); cmdarg::putUInt(out, _rawX);
        out.write(" y=");    cmdarg::putUInt(out, _rawY);
        out.write("  centre x="); cmdarg::putUInt(out, _centerX);
        out.write(" y=");         cmdarg::putUInt(out, _centerY);
        out.write("  full="); cmdarg::putUInt(out, hal_adc_max());
        out.writeln();
        return;
    }
    if (cmdarg::is(p, "cal")) {
        calibrate();
        out.write("centred at x="); cmdarg::putUInt(out, _centerX);
        out.write(" y=");           cmdarg::putUInt(out, _centerY);
        out.writeln();
        return;
    }
    if (cmdarg::is(p, "deadzone")) {
        long v;
        if (!cmdarg::integer(cmdarg::next(p), v, 0, 90)) { usage(out); return; }
        setDeadzone((uint8_t)v);
        out.write("deadzone "); cmdarg::putUInt(out, (uint32_t)v); out.writeln("%");
        return;
    }
    if (cmdarg::is(p, "watch")) {
        _streaming = true;
        out.writeln("streaming direction changes to the board console — 'joy stop' to end");
        return;
    }
    if (cmdarg::is(p, "stop")) { _streaming = false; out.writeln("stopped"); return; }

    if (cmdarg::is(p, "bind")) {
        const char *q = cmdarg::next(p);
        JoyDir d;
        if (!dirFromName(q, d)) { usage(out); return; }
        const char *cmd = cmdarg::next(q);
        if (cmdarg::empty(cmd)) { usage(out); return; }
        strncpy(_bind[(int)d], cmd, kBindLen - 1);
        _bind[(int)d][kBindLen - 1] = '\0';
        out.write("bound "); out.write(joyDirName(d));
        out.write(" -> ");   out.writeln(_bind[(int)d]);
        return;
    }
    if (cmdarg::is(p, "unbind")) {
        JoyDir d;
        if (!dirFromName(cmdarg::next(p), d)) { usage(out); return; }
        _bind[(int)d][0] = '\0';
        out.writeln("unbound");
        return;
    }
    if (cmdarg::is(p, "binds")) {
        bool any = false;
        for (int i = 0; i < kBindMax; i++) {
            if (!_bind[i][0]) continue;
            any = true;
            out.write("  "); out.write(joyDirName((JoyDir)i));
            out.write(" -> "); out.writeln(_bind[i]);
        }
        if (!any) out.writeln("no bindings");
        return;
    }

    if (!cmdarg::empty(p)) { usage(out); return; }

    sample();
    out.write("joy ");  out.write(joyDirName(direction()));
    out.write("  x=");  cmdarg::putInt(out, _x);
    out.write(" y=");   cmdarg::putInt(out, _y);
    if (_sw >= 0) { out.write("  button="); out.write(pressed() ? "down" : "up"); }
    out.writeln();
}

inline void JoystickModule::joyCmd(const char *args, Writer &out, void *ctx) {
    static_cast<JoystickModule *>(ctx)->dispatch(args, out);
}

inline void JoystickModule::registerCommands(CommandRegistry &reg) {
    _reg = &reg;                       // bindings dispatch back through the registry
    reg.registerCommand(CMD(
        "joy", "analog stick: read, cal, deadzone, watch, bind", I2C_NONE,
        joyCmd, this));
}
