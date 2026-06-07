#pragma once
#include "core/IModule.h"
#include "core/CommandRegistry.h"
#include <stdint.h>

// Driver for the six ST7789 (135x240) IPS displays on the IPSTube clock.
//
// The displays share one SPI bus (MOSI/SCLK/DC/RST) and a single backlight line;
// each has its own active-low chip-select GPIO, so esp_lcd drives them as six
// panel_io handles on one bus (CS asserted per transaction). This module is the
// *driver* only — bring-up, backlight, and pixel/fill push. The clock-face logic
// (what digit/image to show) lives in the app: override the weak hook
// `commander_on_ipstube_ready()` to get a reference and drive the displays.
//
// ESP-IDF / esp_lcd; ESP32-only. Implementation in IpstubeModule.cpp keeps all
// esp_lcd/SPI types out of this header so the app can include it freely. Pins
// default to the IPSTube wiring (see the .cpp); override with -DIPSTUBE_PIN_*.
class IpstubeModule : public IModule {
public:
    static constexpr int     kNumDisplays = 6;
    static constexpr int     kWidth       = 135;
    static constexpr int     kHeight      = 240;
    static constexpr uint8_t kAll         = 0xFF;   // address all displays at once

    const char *name() const override { return "ipstube"; }
    void        init() override;       // SPI bus + 6 ST7789 panels + backlight
    void        registerCommands(CommandRegistry &reg) override;
    void        startTask() override {}

    // ── App-facing API (call these from commander_on_ipstube_ready) ───────────
    // Blit a kWidth*kHeight RGB565 framebuffer to one display (0..kNumDisplays-1).
    bool pushDigit(uint8_t display, const uint16_t *fb);
    // Solid-fill one display, or all of them with display == kAll.
    void fill(uint8_t display, uint16_t rgb565);
    void clear(uint8_t display) { fill(display, 0x0000); }
    // Backlight (shared across all six): on/off, or 0..255 PWM duty.
    void backlight(bool on);
    void setBrightness(uint8_t duty);
    bool ready() const { return _ready; }

private:
    bool _ready = false;
    static void cmd(const char *args, Writer &out, void *ctx);
    void        dispatch(const char *args, Writer &out);
    void        usage(Writer &out);
};

// Weak app hook — the generated commander_modules.h calls this after registering
// the module. Override it in your app to wire the clock display logic.
extern "C" void commander_on_ipstube_ready(IpstubeModule &);
