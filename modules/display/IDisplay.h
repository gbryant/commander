#pragma once
#include <stdint.h>

// ── Display vocabulary ───────────────────────────────────────────────────────
// The neutral half of the display split (see docs/writing-a-module.md, "When a
// module can't be fully portable"): panels differ in controller, bus and init
// sequence, but they all end up drawing RGB565 pixels into a rectangle. Apps and
// higher-level UI code target this interface, so a project can change panels
// without changing its screens.
//
// `blit()` is deliberately shaped like LVGL's flush_cb — (area, pixel buffer) —
// so an app that later wants LVGL can bind it in a few lines rather than
// reworking the driver.

// RGB565, the wire format of every panel here: 5 bits red, 6 green, 5 blue.
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

namespace color {
inline constexpr uint16_t kBlack   = rgb565(0, 0, 0);
inline constexpr uint16_t kWhite   = rgb565(255, 255, 255);
inline constexpr uint16_t kRed     = rgb565(255, 0, 0);
inline constexpr uint16_t kGreen   = rgb565(0, 255, 0);
inline constexpr uint16_t kBlue    = rgb565(0, 0, 255);
inline constexpr uint16_t kYellow  = rgb565(255, 255, 0);
inline constexpr uint16_t kCyan    = rgb565(0, 255, 255);
inline constexpr uint16_t kMagenta = rgb565(255, 0, 255);
inline constexpr uint16_t kOrange  = rgb565(255, 140, 0);
inline constexpr uint16_t kGrey    = rgb565(128, 128, 128);
inline constexpr uint16_t kDim     = rgb565(48, 48, 48);

// Look up a colour by name for shell commands ("lcd fill red"). Also accepts a
// raw 16-bit literal as hex ("0xF800") or decimal. Returns false if unparseable,
// leaving `out` untouched — so a typo reports an error instead of painting black.
inline bool parse(const char *s, uint16_t &out) {
    if (!s || !*s) return false;
    struct Named { const char *name; uint16_t value; };
    static constexpr Named kNames[] = {
        {"black", kBlack}, {"white", kWhite},   {"red", kRed},     {"green", kGreen},
        {"blue",  kBlue},  {"yellow", kYellow}, {"cyan", kCyan},   {"magenta", kMagenta},
        {"orange", kOrange}, {"grey", kGrey},   {"gray", kGrey},   {"dim", kDim},
    };
    for (const Named &n : kNames) {
        const char *a = n.name; const char *b = s;
        while (*a && *b && (*a == (*b | 0x20))) { a++; b++; }
        if (!*a && !*b) { out = n.value; return true; }
    }
    // Numeric: 0x-prefixed hex or plain decimal, both bounded to 16 bits.
    uint32_t v = 0; bool any = false;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        for (const char *p = s + 2; *p; p++) {
            int d;
            if (*p >= '0' && *p <= '9')      d = *p - '0';
            else if ((*p | 0x20) >= 'a' && (*p | 0x20) <= 'f') d = (*p | 0x20) - 'a' + 10;
            else return false;
            v = v * 16 + (uint32_t)d; any = true;
            if (v > 0xFFFF) return false;
        }
    } else {
        for (const char *p = s; *p; p++) {
            if (*p < '0' || *p > '9') return false;
            v = v * 10 + (uint32_t)(*p - '0'); any = true;
            if (v > 0xFFFF) return false;
        }
    }
    if (!any) return false;
    out = (uint16_t)v;
    return true;
}
}  // namespace color

class IDisplay {
public:
    virtual ~IDisplay() = default;

    // Size in the CURRENT rotation — a 320x480 panel reports 480x320 in landscape.
    virtual int  width()  const = 0;
    virtual int  height() const = 0;

    virtual void fill(uint16_t color)                                     = 0;
    virtual void fillRect(int x, int y, int w, int h, uint16_t color)     = 0;
    // Copy `pixels` (w*h, row-major, RGB565) into the rectangle at (x, y).
    virtual void blit(int x, int y, int w, int h, const uint16_t *pixels) = 0;

    // Draws with the built-in 5x7 font. `scale` multiplies both axes (1 = 6x8
    // cell). Returns the x coordinate just past the last glyph, so callers can
    // chain runs of different colours on one line.
    virtual int  drawText(int x, int y, const char *s, uint16_t fg, uint16_t bg,
                          int scale = 1)                                  = 0;

    // 0..255. Panels without a controllable backlight pin treat 0 as display-off
    // and anything else as display-on (see St7796Module).
    virtual void backlight(uint8_t level)                                 = 0;

    // Convenience shared by every panel: a one-line status row at text-line
    // `line` (in 8*scale pixel steps), cleared to `bg` across the full width.
    void statusLine(int line, const char *s, uint16_t fg, uint16_t bg, int scale = 2) {
        int h = 8 * scale;
        int y = line * h;
        if (y + h > height()) return;
        fillRect(0, y, width(), h, bg);
        drawText(0, y, s, fg, bg, scale);
    }
};
