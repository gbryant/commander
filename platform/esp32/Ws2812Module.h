#pragma once
#include "core/IModule.h"
#include "core/CommandRegistry.h"
#include <stdint.h>

// Generic WS2812 / SK6812 addressable-RGB driver (one chain), ESP32 via the RMT
// peripheral. The shell command is `wled`; the app drives effects through the
// C++ API (set pixels, then show()) via the weak commander_on_ws2812_ready hook.
//
// A board's onboard RGB LED is just this with count = 1. esp_driver_rmt types are
// kept out of this header (opaque handles) so the app component can include it.
class Ws2812Module : public IModule {
public:
    // Wire order of the chip. WS2812 is GRB; SK6812/others may differ.
    enum Order { GRB, RGB, BRG, RBG, GBR, BGR };

    Ws2812Module(int pin, int count, Order order = GRB)
        : _pin(pin), _count(count), _order(order) {}

    const char *name() const override { return "ws2812"; }
    void        init() override;            // RMT channel + encoder + pixel buffer
    void        registerCommands(CommandRegistry &reg) override;
    void        startTask() override {}

    // ── App-facing API ────────────────────────────────────────────────────────
    int  count() const { return _count; }
    bool ready() const { return _ok; }
    void setPixel(int i, uint8_t r, uint8_t g, uint8_t b);  // buffer only
    void fill(uint8_t r, uint8_t g, uint8_t b);             // buffer only
    void setBrightness(uint8_t b) { _bright = b; }          // applied in show()
    void show();                                            // latch buffer to the strip
    void clear() { fill(0, 0, 0); show(); }

private:
    int     _pin;
    int     _count;
    Order   _order;
    uint8_t _bright = 255;
    bool    _ok     = false;
    void   *_chan   = nullptr;   // rmt_channel_handle_t
    void   *_enc    = nullptr;   // rmt_encoder_handle_t
    uint8_t *_buf   = nullptr;   // count*3, wire order, full-brightness
    uint8_t *_scan  = nullptr;   // count*3, brightness-scaled, sent to RMT

    void wire(uint8_t r, uint8_t g, uint8_t b, uint8_t *dst) const;
    static void cmd(const char *args, Writer &out, void *ctx);
    void        dispatch(const char *args, Writer &out);
    void        usage(Writer &out);
};

// Weak app hook — the generated commander_modules.h calls this after registering.
extern "C" void commander_on_ws2812_ready(Ws2812Module &);
