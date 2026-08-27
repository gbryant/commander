#pragma once
#include "core/IModule.h"
#include "core/CommandRegistry.h"
#include "core/CmdArgs.h"
#include "hal/hal.h"
#include "modules/ConsoleOut.h"
#include <stdint.h>
#include <string.h>

// ── GT911 capacitive touch controller ────────────────────────────────────────
// The touch half of the 3.5" panel on the GeeekPi Pico Breadboard Kit (and a very
// common part generally). Portable: hal_i2c_* only.
//
// The GT911 addresses 16-bit registers, which is why the HAL grew
// hal_i2c_write_read() — a write-STOP-read sequence does not work here, the
// address phase and the read must be one transaction with a repeated start.
//
// The chip reports coordinates in its own panel space (typically 320x480
// portrait). Apps want display space, which changes with display rotation, so
// the module maps between them: setRotation() matches the panel's, and
// swapXY/flipX/flipY handle a touch layer mounted at a different orientation
// from the glass. Everything published through touch() / onTouch is in display
// space; `touch raw` shows the untransformed reading for bring-up.
//
// One shell command, `touch`. Polling happens in tick() — add it as a ticker
// (the cmdr-generated file does) or call read() yourself.

struct TouchPoint {
    int16_t x = 0, y = 0;      // display space
    int16_t rawX = 0, rawY = 0;
    bool    pressed = false;
    uint8_t id = 0;            // GT911 track id
    uint16_t size = 0;
};

class Gt911Module : public IModule {
public:
    // addr: 0x5D (INT low at reset, the usual wiring) or 0x14.
    explicit Gt911Module(uint8_t addr = 0x5D, uint8_t rotation = 0)
        : _addr(addr), _rot(rotation & 3) {}

    const char *name() const override { return "touch"; }

    void init() override {
        // The panel needs a moment after power-up before it answers.
        hal_delay_ms(10);
        uint8_t id[4] = {0, 0, 0, 0};
        if (!readReg(kProductId, id, 4)) { _ok = false; return; }
        memcpy(_productId, id, 4);
        _productId[4] = '\0';

        uint8_t res[4];
        if (readReg(kXResLow, res, 4)) {
            _panelW = (uint16_t)(res[0] | (res[1] << 8));
            _panelH = (uint16_t)(res[2] | (res[3] << 8));
        }
        // A chip that reports nonsense resolution would silently scale every
        // touch to garbage — fall back to the panel geometry we know.
        if (_panelW == 0 || _panelW > 4096) _panelW = 320;
        if (_panelH == 0 || _panelH > 4096) _panelH = 480;
        _ok = true;
    }

    void registerCommands(CommandRegistry &reg) override;

    // Pumped by the UART task; polls the controller and fires onTouch on edges.
    void tick() override {
        uint64_t now = hal_time_us();
        if (now - _lastPoll < kPollUs) return;
        _lastPoll = now;
        TouchPoint p;
        if (!read(p)) return;
        // Press and release always count. Movement only counts once it exceeds
        // a threshold: a finger resting on the glass jitters by a pixel or two
        // every poll, and reporting that as a fresh event makes a consumer
        // re-trigger whatever it does on touch, 60 times a second.
        bool edge  = p.pressed != _last.pressed;
        int  dx    = p.x > _last.x ? p.x - _last.x : _last.x - p.x;
        int  dy    = p.y > _last.y ? p.y - _last.y : _last.y - p.y;
        bool moved = p.pressed && _last.pressed &&
                     (dx >= _moveThreshold || dy >= _moveThreshold);
        bool changed = edge || moved;
        if (changed) _last = p;
        else { _last.pressed = p.pressed; }
        if (changed && _cb) _cb(p, _cbCtx);
        if (changed && _streaming) emitConsole(p);
    }

    // ── App API ──────────────────────────────────────────────────────────────
    bool ready() const { return _ok; }
    const char *productId() const { return _productId; }
    uint16_t panelWidth()  const { return _panelW; }
    uint16_t panelHeight() const { return _panelH; }

    // Reads the controller now. Returns false on a bus error; a successful read
    // with no finger down sets pressed = false.
    bool read(TouchPoint &p) {
        uint8_t status = 0;
        if (!readReg(kStatus, &status, 1)) return false;
        uint8_t points = status & 0x0F;

        if (status & 0x80) {                       // buffer ready → must be cleared
            if (points >= 1) {
                uint8_t d[8];
                if (readReg(kPoint1, d, 8)) {
                    p.id   = d[0];
                    p.rawX = (int16_t)(d[1] | (d[2] << 8));
                    p.rawY = (int16_t)(d[3] | (d[4] << 8));
                    p.size = (uint16_t)(d[5] | (d[6] << 8));
                    p.pressed = true;
                    mapToDisplay(p);
                    _lastGood = p;
                }
            } else {
                p = _lastGood;
                p.pressed = false;
            }
            clearStatus();
            return true;
        }
        // No fresh data: report the last known position, released.
        p = _lastGood;
        p.pressed = false;
        return true;
    }

    TouchPoint last() const { return _last; }

    using Callback = void (*)(const TouchPoint &, void *);
    void onTouch(Callback cb, void *ctx = nullptr) { _cb = cb; _cbCtx = ctx; }

    // Match the display's rotation so touch coordinates line up with what's drawn.
    void setRotation(uint8_t r) { _rot = r & 3; }
    uint8_t rotation() const { return _rot; }
    void setFlip(bool x, bool y, bool swap) { _flipX = x; _flipY = y; _swapXY = swap; }
    // Pixels of movement before a held touch counts as having moved (0 = report
    // every reading, which is rarely what a consumer wants).
    void setMoveThreshold(uint8_t px) { _moveThreshold = px; }

private:
    static constexpr uint16_t kProductId = 0x8140;
    static constexpr uint16_t kXResLow   = 0x8146;
    static constexpr uint16_t kStatus    = 0x814E;
    static constexpr uint16_t kPoint1    = 0x814F;   // track id, then x/y/size
    static constexpr uint64_t kPollUs    = 15000;    // ~66 Hz

    uint8_t    _addr;
    uint8_t    _rot;
    bool       _ok      = false;
    bool       _flipX   = false, _flipY = false, _swapXY = false;
    uint16_t   _panelW  = 320, _panelH = 480;
    char       _productId[5] = {0, 0, 0, 0, 0};
    TouchPoint _last, _lastGood;
    uint64_t   _lastPoll = 0;
    Callback   _cb    = nullptr;
    void      *_cbCtx = nullptr;
    bool       _streaming = false;
    uint8_t    _moveThreshold = 4;

    bool readReg(uint16_t reg, uint8_t *buf, size_t len) {
        uint8_t a[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
        return hal_i2c_write_read(_addr, a, 2, buf, len);
    }
    void clearStatus() {
        uint8_t w[3] = {(uint8_t)(kStatus >> 8), (uint8_t)(kStatus & 0xFF), 0x00};
        hal_i2c_write_read(_addr, w, 3, nullptr, 0);
    }

    // Panel space → display space. Rotation follows the same 90°-per-step
    // convention as St7796Module, so `touch` and `lcd` agree at every rotation.
    void mapToDisplay(TouchPoint &p) const {
        int x = p.rawX, y = p.rawY;
        int w = _panelW, h = _panelH;
        if (_flipX)  x = w - 1 - x;
        if (_flipY)  y = h - 1 - y;
        if (_swapXY) { int t = x; x = y; y = t; int tw = w; w = h; h = tw; }
        switch (_rot) {
            case 1:  { int t = x; x = h - 1 - y; y = t; break; }   // 90°
            case 2:  { x = w - 1 - x; y = h - 1 - y; break; }      // 180°
            case 3:  { int t = y; y = w - 1 - x; x = t; break; }   // 270°
            default: break;
        }
        p.x = (int16_t)x;
        p.y = (int16_t)y;
    }

    // Streamed from tick() → the board console, never a stored Writer
    // (see modules/ConsoleOut.h for why).
    static void emitConsole(const TouchPoint &p) {
        if (p.pressed) {
            console::puts("touch ");
            console::putInt(p.x); console::puts(",");
            console::putInt(p.y);
            console::puts("  raw ");
            console::putInt(p.rawX); console::puts(",");
            console::putInt(p.rawY);
            console::endl();
        } else {
            console::puts("release");
            console::endl();
        }
    }

    static void emit(const TouchPoint &p, Writer &out) {
        if (p.pressed) {
            out.write("touch ");
            cmdarg::putInt(out, p.x); out.write(",");
            cmdarg::putInt(out, p.y);
            out.write("  raw ");
            cmdarg::putInt(out, p.rawX); out.write(",");
            cmdarg::putInt(out, p.rawY);
            out.writeln();
        } else {
            out.writeln("release");
        }
    }

    static void touchCmd(const char *args, Writer &out, void *ctx);
    void        dispatch(const char *args, Writer &out);
    void        usage(Writer &out);
};

// Weak app hook — the generated commander_modules.h null-checks and calls this.
extern "C" void commander_on_touch_ready(Gt911Module &) __attribute__((weak));

// ─────────────────────────────────────────────────────────────────────────────

inline void Gt911Module::usage(Writer &out) {
    out.writeln("touch                 read the panel once");
    out.writeln("touch info            controller id, resolution, mapping");
    out.writeln("touch raw             read once, untransformed");
    out.writeln("touch watch           stream points to the board console");
    out.writeln("touch stop            stop streaming");
    out.writeln("touch rotate <0-3>    match the display rotation");
    out.writeln("touch flip x|y|xy|none");
}

inline void Gt911Module::dispatch(const char *args, Writer &out) {
    const char *p = cmdarg::skipSpaces(args);

    if (cmdarg::is(p, "help")) { usage(out); return; }

    if (cmdarg::is(p, "info")) {
        out.writeln("gt911 touch");
        cmdarg::putField(out, "product", _productId[0] ? _productId : "(no answer)");
        out.write("  address: 0x"); cmdarg::putHex8(out, _addr); out.writeln();
        out.write("  panel: ");
        cmdarg::putUInt(out, _panelW); out.write("x"); cmdarg::putUInt(out, _panelH);
        out.writeln();
        cmdarg::putField(out, "rotation", _rot);
        out.write("  flip: x="); out.write(_flipX ? "yes" : "no");
        out.write(" y=");        out.write(_flipY ? "yes" : "no");
        out.write(" swap=");     out.write(_swapXY ? "yes" : "no");
        out.writeln();
        cmdarg::putField(out, "state", _ok ? "ok" : "not responding");
        return;
    }
    if (cmdarg::is(p, "watch")) {
        _streaming = true;
        out.writeln("streaming touches to the board console — 'touch stop' to end");
        return;
    }
    if (cmdarg::is(p, "stop")) {
        _streaming = false;
        out.writeln("stopped");
        return;
    }
    if (cmdarg::is(p, "rotate")) {
        long r;
        if (!cmdarg::integer(cmdarg::next(p), r, 0, 3)) { usage(out); return; }
        setRotation((uint8_t)r);
        out.write("rotation "); cmdarg::putUInt(out, (uint32_t)r); out.writeln();
        return;
    }
    if (cmdarg::is(p, "flip")) {
        const char *q = cmdarg::next(p);
        if      (cmdarg::is(q, "x"))    setFlip(true,  false, _swapXY);
        else if (cmdarg::is(q, "y"))    setFlip(false, true,  _swapXY);
        else if (cmdarg::is(q, "xy"))   setFlip(true,  true,  _swapXY);
        else if (cmdarg::is(q, "swap")) setFlip(_flipX, _flipY, !_swapXY);
        else if (cmdarg::is(q, "none")) setFlip(false, false, false);
        else { usage(out); return; }
        out.writeln("ok");
        return;
    }

    bool raw = cmdarg::is(p, "raw");
    if (!cmdarg::empty(p) && !raw) { usage(out); return; }

    TouchPoint pt;
    if (!read(pt)) { out.writeln("touch: no answer from the controller"); return; }
    if (!pt.pressed) { out.writeln("no touch"); return; }
    if (raw) {
        out.write("raw "); cmdarg::putInt(out, pt.rawX);
        out.write(",");    cmdarg::putInt(out, pt.rawY);
        out.write("  size "); cmdarg::putUInt(out, pt.size);
        out.writeln();
    } else {
        emit(pt, out);
    }
}

inline void Gt911Module::touchCmd(const char *args, Writer &out, void *ctx) {
    static_cast<Gt911Module *>(ctx)->dispatch(args, out);
}

inline void Gt911Module::registerCommands(CommandRegistry &reg) {
    reg.registerCommand(CMD(
        "touch", "capacitive touch: read, watch, rotate, flip", I2C_NONE,
        touchCmd, this));
}
