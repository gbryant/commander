#pragma once
#include "core/IModule.h"
#include "core/CommandRegistry.h"
#include "modules/controller/ControllerState.h"
#include "modules/controller/ControllerBackend.h"
#include <string.h>

// Generic game-controller input module. Knows nothing about robots, locomotion,
// or any application — it just publishes controller input and lets a project
// wire it to whatever, three ways that mix and match freely:
//
//   • Poll   — state() returns the latest snapshot; read it from your own tick().
//              Best for analog/continuous control (sticks → motion).
//   • Push   — onUpdate()/onButton() register C++ callbacks (button edges + every
//              sample). Best for event-driven or custom logic.
//   • Bind   — `bind <button> <command…>` maps a button to any registered shell
//              command, dispatched through the CommandRegistry on press. Best for
//              digital actions with no app code: `bind a drive fwd 200`.
//
// A ControllerBackend (Bluepad32 on the Pico, etc.) feeds it via update().
#ifndef CONTROLLER_MAX_LISTENERS
#define CONTROLLER_MAX_LISTENERS 4
#endif
#ifndef CONTROLLER_BIND_MAXLEN
#define CONTROLLER_BIND_MAXLEN 48
#endif

class ControllerModule : public IModule {
public:
    using UpdateFn = void (*)(const ControllerState &, void *ctx);
    using ButtonFn = void (*)(GamepadButton, bool pressed, void *ctx);

    explicit ControllerModule(ControllerBackend &backend) : _backend(backend) {}

    const char *name() const override { return "controller"; }
    void        init() override { _backend.begin(*this); }
    void        registerCommands(CommandRegistry &reg) override;

    // ── consumer plumbing ────────────────────────────────────────────────────
    const ControllerState &state() const { return _state; }

    // Push: returns false if the listener table is full. `name` is an optional
    // static label shown by `pad` so wired callbacks are inspectable (like
    // bindings) — a registered listener is otherwise an opaque function pointer.
    bool onUpdate(UpdateFn fn, void *ctx = nullptr, const char *name = nullptr) {
        if (_nUpdate >= CONTROLLER_MAX_LISTENERS) return false;
        _updateSubs[_nUpdate++] = {fn, ctx, name};
        return true;
    }
    bool onButton(ButtonFn fn, void *ctx = nullptr, const char *name = nullptr) {
        if (_nButton >= CONTROLLER_MAX_LISTENERS) return false;
        _buttonSubs[_nButton++] = {fn, ctx, name};
        return true;
    }

    // Declarative bindings (also settable from app code, not just the shell).
    bool bindButton(GamepadButton b, const char *cmdline) {
        if (b >= GAMEPAD_BUTTON_COUNT) return false;
        strncpy(_bindings[b], cmdline, CONTROLLER_BIND_MAXLEN - 1);
        _bindings[b][CONTROLLER_BIND_MAXLEN - 1] = '\0';
        return true;
    }
    void unbindButton(GamepadButton b) {
        if (b < GAMEPAD_BUTTON_COUNT) _bindings[b][0] = '\0';
    }

    // Where bound-command output goes (default: discarded). A runner can route
    // this to the UART writer to echo bound-command results on the console.
    void setOutput(Writer &out) { _out = &out; }

    // ── backend entry point ──────────────────────────────────────────────────
    // The backend calls this on each fresh sample (its own task context). Detects
    // button edges (firing button listeners + bindings on press), then notifies
    // update listeners. Runs synchronously — keep listeners short.
    void update(const ControllerState &s) {
        uint32_t changed = s.buttons ^ _prevButtons;
        _state = s;
        if (changed) {
            for (uint8_t b = 0; b < GAMEPAD_BUTTON_COUNT; b++) {
                uint32_t mask = 1u << b;
                if (!(changed & mask)) continue;
                bool pressed = (s.buttons & mask) != 0;
                for (size_t i = 0; i < _nButton; i++)
                    _buttonSubs[i].fn((GamepadButton)b, pressed, _buttonSubs[i].ctx);
                if (pressed) dispatchBinding((GamepadButton)b);
            }
        }
        _prevButtons = s.buttons;
        for (size_t i = 0; i < _nUpdate; i++)
            _updateSubs[i].fn(s, _updateSubs[i].ctx);
    }

private:
    ControllerBackend &_backend;
    ControllerState    _state;
    uint32_t           _prevButtons = 0;
    CommandRegistry   *_reg = nullptr;
    Writer            *_out = nullptr;

    struct UpdateSub { UpdateFn fn; void *ctx; const char *name; };
    struct ButtonSub { ButtonFn fn; void *ctx; const char *name; };
    UpdateSub _updateSubs[CONTROLLER_MAX_LISTENERS]; size_t _nUpdate = 0;
    ButtonSub _buttonSubs[CONTROLLER_MAX_LISTENERS]; size_t _nButton = 0;

    char _bindings[GAMEPAD_BUTTON_COUNT][CONTROLLER_BIND_MAXLEN] = {};

    // Discards bound-command output unless the app calls setOutput().
    struct NullWriter : Writer {
        void write(const char *) override {}
        void writeln(const char * = "") override {}
    };

    void dispatchBinding(GamepadButton b) {
        if (!_reg || _bindings[b][0] == '\0') return;
        static NullWriter null;
        _reg->dispatch(_bindings[b], _out ? *_out : null);
    }

    static void padCmd(const char *args, Writer &out, void *ctx);
    static void bindCmd(const char *args, Writer &out, void *ctx);
    static void unbindCmd(const char *args, Writer &out, void *ctx);
    static void btforgetCmd(const char *args, Writer &out, void *ctx);

    static const char *skipSpaces(const char *p) { while (*p == ' ') ++p; return p; }
    static const char *tokEnd(const char *p) { while (*p && *p != ' ') ++p; return p; }
    static void putUInt(Writer &out, uint32_t v) {
        char tmp[11]; int t = 0;
        if (v == 0) tmp[t++] = '0';
        while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
        char s[12]; int i = 0;
        while (t) s[i++] = tmp[--t];
        s[i] = '\0';
        out.write(s);
    }
    static void putInt(Writer &out, int32_t v) {
        if (v < 0) { out.write("-"); putUInt(out, (uint32_t)(-(int64_t)v)); }
        else putUInt(out, (uint32_t)v);
    }
};

inline void ControllerModule::registerCommands(CommandRegistry &reg) {
    _reg = &reg;
    reg.registerCommand(CMD("pad",    "Controller status + bindings - 'pad'",       I2C_NONE, padCmd,    this));
    reg.registerCommand(CMD("bind",   "bind <button> <command…> - button -> command", I2C_NONE, bindCmd,   this));
    reg.registerCommand(CMD("unbind", "unbind <button> - remove a binding",         I2C_NONE, unbindCmd, this));
    reg.registerCommand(CMD("btforget", "clear BT pairings so a controller can re-pair", I2C_NONE, btforgetCmd, this));
}

inline void ControllerModule::btforgetCmd(const char *args, Writer &out, void *ctx) {
    (void)args;
    ControllerModule *self = static_cast<ControllerModule *>(ctx);
    self->_backend.forgetKeys();
    out.writeln("cleared BT pairings; scanning. Put the controller in pairing mode now.");
}

inline void ControllerModule::padCmd(const char *args, Writer &out, void *ctx) {
    (void)args;
    ControllerModule *self = static_cast<ControllerModule *>(ctx);
    const ControllerState &s = self->_state;
    out.write("controller: "); out.writeln(s.connected ? "connected" : "disconnected");
    out.write("  L("); putInt(out, s.lx); out.write(","); putInt(out, s.ly);
    out.write(")  R("); putInt(out, s.rx); out.write(","); putInt(out, s.ry);
    out.write(")  LT="); putUInt(out, s.lt); out.write(" RT="); putUInt(out, s.rt);
    out.writeln();
    out.write("  pressed:");
    bool any = false;
    for (uint8_t b = 0; b < GAMEPAD_BUTTON_COUNT; b++)
        if (s.pressed((GamepadButton)b)) { out.write(" "); out.write(gamepad_button_name((GamepadButton)b)); any = true; }
    out.writeln(any ? "" : " (none)");
    out.writeln("bindings:");
    bool anyBind = false;
    for (uint8_t b = 0; b < GAMEPAD_BUTTON_COUNT; b++)
        if (self->_bindings[b][0]) {
            out.write("  "); out.write(gamepad_button_name((GamepadButton)b));
            out.write(" -> "); out.writeln(self->_bindings[b]); anyBind = true;
        }
    if (!anyBind) out.writeln("  (none)");

    // Registered callbacks — counts + optional labels, so wired listeners are
    // visible even though a function pointer has no name of its own.
    out.write("listeners: update="); putUInt(out, self->_nUpdate);
    out.write(" button="); putUInt(out, self->_nButton); out.writeln();
    for (size_t i = 0; i < self->_nUpdate; i++) {
        out.write("  update -> ");
        out.writeln(self->_updateSubs[i].name ? self->_updateSubs[i].name : "(unnamed)");
    }
    for (size_t i = 0; i < self->_nButton; i++) {
        out.write("  button -> ");
        out.writeln(self->_buttonSubs[i].name ? self->_buttonSubs[i].name : "(unnamed)");
    }
}

inline void ControllerModule::bindCmd(const char *args, Writer &out, void *ctx) {
    ControllerModule *self = static_cast<ControllerModule *>(ctx);
    const char *p = skipSpaces(args);
    const char *e = tokEnd(p);
    if (e == p) { out.writeln("usage: bind <button> <command…>"); return; }
    GamepadButton b = gamepad_button_from_name(p, (size_t)(e - p));
    if (b >= GAMEPAD_BUTTON_COUNT) { out.write("unknown button: "); out.writeln(p); return; }
    const char *cmd = skipSpaces(e);
    if (*cmd == '\0') { out.writeln("usage: bind <button> <command…>"); return; }
    self->bindButton(b, cmd);
    out.write("bound "); out.write(gamepad_button_name(b)); out.write(" -> "); out.writeln(cmd);
}

inline void ControllerModule::unbindCmd(const char *args, Writer &out, void *ctx) {
    ControllerModule *self = static_cast<ControllerModule *>(ctx);
    const char *p = skipSpaces(args);
    const char *e = tokEnd(p);
    GamepadButton b = gamepad_button_from_name(p, (size_t)(e - p));
    if (b >= GAMEPAD_BUTTON_COUNT) { out.write("unknown button: "); out.writeln(p); return; }
    self->unbindButton(b);
    out.write("unbound "); out.writeln(gamepad_button_name(b));
}
