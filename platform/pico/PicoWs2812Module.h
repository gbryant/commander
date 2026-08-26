#pragma once
#include "core/IModule.h"
#include "core/CommandRegistry.h"
#include <stdint.h>

// ── WS2812 / SK6812 addressable RGB on Pico (PIO) ────────────────────────────
// The Pico-side twin of platform/esp32/Ws2812Module: same `wled` command surface,
// same C++ API, same commander_on_ws2812_ready() hook — different peripheral
// underneath (PIO state machine here, RMT there). App code that drives LEDs
// moves between the two boards unchanged.
//
// PIO types are kept out of this header (the state machine lives in the .cpp),
// so the cmdr-generated file and app code can include it with no PIO wiring.
//
// A board's single onboard RGB LED is just this with count = 1 — which is exactly
// the GeeekPi Pico Breadboard Kit's RGB LED on GP12.
class PicoWs2812Module : public IModule {
public:
    // Wire order of the chip. WS2812 is GRB; SK6812 and others may differ.
    enum Order { GRB, RGB, BRG, RBG, GBR, BGR };

    static constexpr int kMaxPixels = 64;   // bounded: no heap on the LED path

    PicoWs2812Module(int pin, int count, Order order = GRB)
        : _pin(pin), _count(count > kMaxPixels ? kMaxPixels : count), _order(order) {}

    const char *name() const override { return "ws2812"; }
    void        init() override;
    void        registerCommands(CommandRegistry &reg) override;

    // ── App-facing API ───────────────────────────────────────────────────────
    int  count() const { return _count; }
    bool ready() const { return _ok; }
    void setPixel(int i, uint8_t r, uint8_t g, uint8_t b);   // buffer only
    void fill(uint8_t r, uint8_t g, uint8_t b);              // buffer only
    void setBrightness(uint8_t b) { _bright = b; }           // applied in show()
    void show();                                             // latch to the strip
    void clear() { fill(0, 0, 0); show(); }

private:
    int     _pin;
    int     _count;
    Order   _order;
    uint8_t _bright = 255;
    bool    _ok     = false;
    uint8_t _buf[kMaxPixels * 3] = {};    // full-brightness, wire order

    void wire(uint8_t r, uint8_t g, uint8_t b, uint8_t *dst) const;

    static void cmd(const char *args, Writer &out, void *ctx);
    void        dispatch(const char *args, Writer &out);
    void        usage(Writer &out);
};

// Weak app hook — the generated commander_modules.h calls this after registering.
extern "C" void commander_on_ws2812_ready(PicoWs2812Module &) __attribute__((weak));
