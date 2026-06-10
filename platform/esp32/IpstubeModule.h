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
    // Blit a w*h RGB565 region to (x,y) on one display, or every display with
    // display == kAll. Coordinates are in panel pixels (0,0 = top-left); the
    // panel gap/offset is applied for you. data is w*h little-endian RGB565.
    bool drawBitmap(uint8_t display, int x, int y, int w, int h, const uint16_t *data);
    // Full-screen convenience — blit a kWidth*kHeight framebuffer.
    bool drawBitmap(uint8_t display, const uint16_t *data) {
        return drawBitmap(display, 0, 0, kWidth, kHeight, data);
    }
    // Solid-fill one display, or all of them with display == kAll.
    void fill(uint8_t display, uint16_t rgb565);
    void clear(uint8_t display) { fill(display, 0x0000); }

    // ── Text rendering (stb_truetype) ─────────────────────────────────────────
    // Scalable, antialiased text drawn through the same RGB565 path as
    // drawBitmap — purely additive to the bitmap clock. Install a font once
    // (the app embeds the .ttf and passes it here), then draw with a TextStyle.
    struct TextStyle {
        int      px     = 120;             // glyph pixel height
        uint16_t fg     = 0xFFFF;          // RGB565 foreground
        uint16_t bg     = 0x0000;          // RGB565 background (target cleared to this)
        enum HAlign { Left, Center, Right }  halign = Center;
        enum VAlign { Top, Middle, Bottom }  valign = Middle;
    };

    // Install the TTF the text API renders with. `ttf` must stay valid for as
    // long as text is drawn (point it at the EMBED_FILES blob). false = unparseable.
    bool loadFont(const uint8_t *ttf, size_t len);
    bool haveFont() const { return _font_ok; }

    // Pixel extent of `str` at style.px. Width spans the glyph advances; height is
    // style.px (the line box). Either out-pointer may be null.
    void measureText(const char *str, const TextStyle &s, int *w, int *h);

    // Largest px at which `str` fits a boxW*boxH box on one line (width measured
    // from glyph advances, height = px). Capped at maxPx. One measurement — px
    // scales linearly with width. Returns 0 with no font / empty string. The
    // reusable primitive behind size-to-fit (and, later, wrap/flow).
    int fitPx(const char *str, int boxW, int boxH, int maxPx = kHeight) const;

    // Size-to-fit convenience: pick the px that fills the panel (inset by `pad`
    // on every edge) and draw `str` centered. Mutates a copy of `s` (px/align).
    bool drawTextFit(uint8_t display, const char *str, TextStyle s, int pad = 8);

    // Word-wrapped multi-line text within a panel (inset by `pad`). With s.px<=0,
    // the size is auto-fit so all wrapped lines fill the panel; otherwise s.px is
    // used as-is. Lines align per s.halign; the stack places per s.valign.
    bool drawTextWrapped(uint8_t display, const char *str, TextStyle s, int pad = 8);

    // Static cross-panel flow: lay `str` left-to-right across the six panels, one
    // word-wrapped line per panel (whole words/characters only — never split a
    // glyph across the bezel). With s.px<=0 the size is auto-fit to flow within as
    // few panels as possible (≤6); otherwise s.px is used. Each panel's line is
    // aligned per s.halign and placed per s.valign. Returns the panels used (≤6;
    // text longer than 6 panels at the minimum size is truncated).
    int drawTextFlow(const char *str, TextStyle s, int pad = 8);

    // Draw `str` on one display (or all with display==kAll). (ax,ay) is the panel-
    // local anchor; alignment places the text box around it (Center/Middle =
    // centered on that point). The panel is cleared to s.bg first. Pixels off the
    // panel are clipped, so ax may be negative or past kWidth for sliding text.
    bool drawText(uint8_t display, int ax, int ay, const char *str, const TextStyle &s);
    // Convenience: centered on the panel.
    bool drawText(uint8_t display, const char *str, const TextStyle &s) {
        return drawText(display, kWidth / 2, kHeight / 2, str, s);
    }

    // Strip mode — treat the six panels as one stripWidth()-wide canvas (panel i
    // owns virtual x [i*kWidth, (i+1)*kWidth)). Renders `str` at strip anchor
    // (ax,ay), clears the whole strip to s.bg, and blits all six. ax may be < 0 or
    // > stripWidth(): sweep it across frames for a marquee. Honors halign/valign
    // about (ax,ay) just like drawText.
    void drawTextStrip(int ax, int ay, const char *str, const TextStyle &s);
    static constexpr int stripWidth() { return kNumDisplays * kWidth; }

    // Animation-optimized marquee. Same visual as sweeping drawTextStrip's ax, but
    // the message is rasterized once into a cached band (rebuilt only when the
    // text/style changes) and each frame just windows + blits that band — no
    // per-frame glyph rasterizing, and only the text rows are transferred. Call it
    // in a loop with a decreasing ax (text is vertically centered on the panels).
    void drawMarquee(int ax, const char *str, const TextStyle &s);

    // Single-panel vertical scroll — `str` word-wrapped to the panel width and
    // rendered once into a cached column; each frame shows a kHeight window at
    // vertical offset `ay` (bg outside the column). Sweep ay from -kHeight up to
    // vscrollHeight() to creep the text upward (a single panel is cheap → smooth).
    // display may be kAll to mirror the same scroll on every panel.
    void drawVScroll(uint8_t display, int ay, const char *str, const TextStyle &s);
    // Total column height for `str` at `s` — the ay sweep range. (Builds the cache.)
    int  vscrollHeight(const char *str, const TextStyle &s);
    // Backlight (shared across all six): on/off, or 0..255 PWM duty.
    void backlight(bool on);
    void setBrightness(uint8_t duty);
    bool ready() const { return _ready; }

private:
    bool _ready   = false;
    bool _font_ok = false;
    static void cmd(const char *args, Writer &out, void *ctx);
    void        dispatch(const char *args, Writer &out);
    void        usage(Writer &out);
};

// Weak app hook — the generated commander_modules.h calls this after registering
// the module. Override it in your app to wire the clock display logic.
extern "C" void commander_on_ipstube_ready(IpstubeModule &);
