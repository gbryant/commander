#pragma once
#include "core/IModule.h"
#include "core/CommandRegistry.h"
#include "core/CmdArgs.h"
#include "hal/hal.h"
#include "modules/display/IDisplay.h"
#include "modules/display/Font5x7.h"
#include <string.h>

// ── ST7796S / ST7796SU1 TFT panel over SPI ───────────────────────────────────
// The 3.5" 320x480 panel on the GeeekPi Pico Breadboard Kit, and the same
// controller on plenty of other boards. Portable: it talks hal_spi_* + hal_gpio_*
// + hal_pwm_* only, so any platform whose HAL implements those gets it.
//
// Chip-select and data/command are driven as plain GPIO rather than by the SPI
// block, because the panel's CS/DC timing is the driver's business, not the
// bus's (see the hal_spi_* comment in hal/hal.h).
//
// One shell command, `lcd`. Apps draw through the IDisplay interface handed to
// the weak commander_on_display_ready() hook — deliberately the *interface*, not
// this class, so app screens survive a change of panel.
//
// Memory: one row buffer of kMaxRow pixels (960 bytes at 480 wide). There is no
// framebuffer — 320x480x2 = 300 KB doesn't fit, so drawing goes straight out the
// wire. That's why there is no read-back or scroll: the panel holds the only copy.

struct St7796Config {
    uint8_t  bus      = 0;      // SPI controller (pins decide which; 0 = spi0)
    int8_t   sck      = 2;
    int8_t   mosi     = 3;
    int8_t   cs       = 5;
    int8_t   dc       = 6;
    int8_t   rst      = 7;
    int8_t   bl       = -1;     // PWM backlight pin, -1 = not wired (kit ties it on)
    uint16_t nativeW  = 320;    // panel's native portrait geometry
    uint16_t nativeH  = 480;
    uint8_t  rotation = 0;      // 0..3, 90° per step
    uint32_t hz       = 40000000;
    bool     invert   = true;   // this panel ships inverted (vendor code sends INVON)
};

class St7796Module : public IModule, public IDisplay {
public:
    explicit St7796Module(const St7796Config &cfg) : _cfg(cfg), _rot(cfg.rotation & 3) {}

    const char *name() const override { return "lcd"; }

    void init() override {
        if (_cfg.cs  >= 0) { hal_gpio_set_output((uint8_t)_cfg.cs);  hal_gpio_write((uint8_t)_cfg.cs, true); }
        if (_cfg.dc  >= 0) { hal_gpio_set_output((uint8_t)_cfg.dc);  hal_gpio_write((uint8_t)_cfg.dc, true); }
        if (_cfg.rst >= 0)   hal_gpio_set_output((uint8_t)_cfg.rst);
        if (_cfg.bl  >= 0)   hal_pwm_init((uint8_t)_cfg.bl);

        hal_spi_init(_cfg.bus, _cfg.sck, _cfg.mosi, -1, _cfg.hz);
        reset();
        sendInitSequence();
        setRotation(_rot);
        cmd(_cfg.invert ? 0x21 : 0x20);          // INVON / INVOFF
        cmd(0x11); hal_delay_ms(120);            // SLPOUT
        cmd(0x29); hal_delay_ms(20);             // DISPON
        _on = true;
        fill(color::kBlack);
        backlight(255);
        _ready = true;
    }

    void registerCommands(CommandRegistry &reg) override;

    // ── IDisplay ─────────────────────────────────────────────────────────────
    int width()  const override { return (_rot & 1) ? _cfg.nativeH : _cfg.nativeW; }
    int height() const override { return (_rot & 1) ? _cfg.nativeW : _cfg.nativeH; }

    void fill(uint16_t c) override { fillRect(0, 0, width(), height(), c); }

    void fillRect(int x, int y, int w, int h, uint16_t c) override {
        if (!clip(x, y, w, h)) return;
        setWindow(x, y, w, h);
        // The row buffer bounds one push, not the rectangle: a panel configured
        // wider than kMaxRow still fills correctly, in chunks.
        const int chunk = w < kMaxRow ? w : kMaxRow;
        for (int i = 0; i < chunk; i++) _row[i] = c;
        for (int r = 0; r < h; r++) {
            int remaining = w;
            while (remaining > 0) {
                int n = remaining < chunk ? remaining : chunk;
                pushPixels(_row, n);
                remaining -= n;
            }
        }
        endWrite();
    }

    void blit(int x, int y, int w, int h, const uint16_t *pixels) override {
        if (!pixels) return;
        // Clipping a blit means skipping rows/columns of the source, so do it by
        // hand rather than through clip() — a partially off-screen sprite is a
        // normal thing for an app to draw.
        int sx = 0, sy = 0, sw = w;
        if (x < 0) { sx = -x; w += x; x = 0; }
        if (y < 0) { sy = -y; h += y; y = 0; }
        if (x >= width() || y >= height() || w <= 0 || h <= 0) return;
        if (x + w > width())  w = width()  - x;
        if (y + h > height()) h = height() - y;
        setWindow(x, y, w, h);
        for (int r = 0; r < h; r++) pushPixels(pixels + (size_t)(sy + r) * sw + sx, w);
        endWrite();
    }

    int drawText(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale = 1) override {
        if (!s || !*s) return x;
        if (scale < 1) scale = 1;
        const int cellW = font5x7::kCellW * scale;
        const int cellH = font5x7::kCellH * scale;
        int n = (int)strlen(s);
        int w = n * cellW;
        int h = cellH;
        int endX = x + w;
        // Clip to the panel. A negative origin can't just be clamped — the
        // address window would go negative and the panel would be handed
        // nonsense — so the hidden rows/columns are skipped in the glyph lookup
        // instead, exactly as blit() skips hidden source pixels.
        int x0 = x, y0 = y;
        int skipX = 0, skipY = 0;
        if (x0 < 0) { skipX = -x0; w -= skipX; x0 = 0; }
        if (y0 < 0) { skipY = -y0; h -= skipY; y0 = 0; }
        if (y0 >= height() || x0 >= width()) return endX;
        if (y0 + h > height()) h = height() - y0;
        if (x0 + w > width())  w = width()  - x0;
        if (w <= 0 || h <= 0) return endX;
        if (w > kMaxRow) w = kMaxRow;

        setWindow(x0, y0, w, h);
        for (int row = 0; row < h; row++) {
            int gy = (row + skipY) / scale;             // glyph row 0..7
            for (int col = 0; col < w; col++) {
                int gx  = (col + skipX) / scale;        // pixel column in the run
                int ci  = gx / font5x7::kCellW;         // character index
                int cx  = gx % font5x7::kCellW;         // column within the cell
                bool on = false;
                if (gy < font5x7::kHeight && cx < font5x7::kWidth)
                    on = (font5x7::column(s[ci], cx) >> gy) & 1;
                _row[col] = on ? fg : bg;
            }
            pushPixels(_row, w);
        }
        endWrite();
        return endX;
    }

    void backlight(uint8_t level) override {
        _bl = level;
        if (_cfg.bl >= 0) { hal_pwm_duty((uint8_t)_cfg.bl, level); return; }
        // No backlight pin (the kit hard-wires it on): fall back to the panel's
        // own display on/off so `lcd bl 0` still visibly does something.
        bool want = level > 0;
        if (want != _on) { cmd(want ? 0x29 : 0x28); _on = want; }
    }

    // ── Panel-specific extras (not in IDisplay) ──────────────────────────────
    bool ready() const { return _ready; }
    void setRotation(uint8_t r) {
        static const uint8_t kMadctl[4] = {0x48, 0x28, 0x88, 0xE8};  // 90° per step
        _rot = r & 3;
        cmd(0x36);
        uint8_t v = kMadctl[_rot];
        data(&v, 1);
    }
    uint8_t rotation() const { return _rot; }
    void setInvert(bool on) { _cfg.invert = on; cmd(on ? 0x21 : 0x20); }
    void sleep(bool on) {
        cmd(on ? 0x10 : 0x11);
        hal_delay_ms(on ? 5 : 120);
        _on = !on;
    }
    // A visible end-to-end check: colour bars, a border, and text at three
    // scales. This is the first thing to run when the hardware lands.
    void selfTest();

private:
    static constexpr int kMaxRow = 480;   // longest panel edge we buffer a row of

    St7796Config _cfg;
    uint8_t      _rot   = 0;
    uint8_t      _bl    = 255;
    bool         _on    = false;
    bool         _ready = false;
    uint16_t     _row[kMaxRow] = {};

    // ── Wire level ───────────────────────────────────────────────────────────
    void select(bool on)  { if (_cfg.cs >= 0) hal_gpio_write((uint8_t)_cfg.cs, !on); }
    void dcCommand()      { if (_cfg.dc >= 0) hal_gpio_write((uint8_t)_cfg.dc, false); }
    void dcData()         { if (_cfg.dc >= 0) hal_gpio_write((uint8_t)_cfg.dc, true); }

    void cmd(uint8_t c) {
        dcCommand(); select(true);
        hal_spi_write(_cfg.bus, &c, 1);
        select(false);
    }
    void data(const uint8_t *d, size_t n) {
        if (!n) return;
        dcData(); select(true);
        hal_spi_write(_cfg.bus, d, n);
        select(false);
    }
    // Pixel pushes keep CS asserted across the whole window write (endWrite()
    // releases it) — releasing per row would cost a GPIO round trip per 480 px.
    void pushPixels(const uint16_t *px, int count) {
        dcData(); select(true);
        hal_spi_write16(_cfg.bus, px, (size_t)count);
    }
    void endWrite() { select(false); }

    void reset() {
        if (_cfg.rst < 0) return;
        hal_gpio_write((uint8_t)_cfg.rst, true);  hal_delay_ms(20);
        hal_gpio_write((uint8_t)_cfg.rst, false); hal_delay_ms(20);
        hal_gpio_write((uint8_t)_cfg.rst, true);  hal_delay_ms(120);
    }

    void setWindow(int x, int y, int w, int h) {
        uint8_t b[4];
        int x2 = x + w - 1, y2 = y + h - 1;
        cmd(0x2A);                                    // CASET
        b[0] = (uint8_t)(x >> 8);  b[1] = (uint8_t)x;
        b[2] = (uint8_t)(x2 >> 8); b[3] = (uint8_t)x2;
        data(b, 4);
        cmd(0x2B);                                    // RASET
        b[0] = (uint8_t)(y >> 8);  b[1] = (uint8_t)y;
        b[2] = (uint8_t)(y2 >> 8); b[3] = (uint8_t)y2;
        data(b, 4);
        cmd(0x2C);                                    // RAMWR
    }

    bool clip(int &x, int &y, int &w, int &h) const {
        if (w <= 0 || h <= 0) return false;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x >= width() || y >= height()) return false;
        if (x + w > width())  w = width()  - x;
        if (y + h > height()) h = height() - y;
        return w > 0 && h > 0;
    }

    void sendInitSequence();

    // ── Shell ────────────────────────────────────────────────────────────────
    static void lcdCmd(const char *args, Writer &out, void *ctx);
    void        dispatch(const char *args, Writer &out);
    void        usage(Writer &out);
    void        info(Writer &out);
};

// Weak app hook — the generated commander_modules.h null-checks and calls this
// after registering. It hands over the IDisplay interface rather than the
// concrete driver so app screens don't bind to one panel.
extern "C" void commander_on_display_ready(IDisplay &) __attribute__((weak));

// ─────────────────────────────────────────────────────────────────────────────

inline void St7796Module::sendInitSequence() {
#ifdef ST7796_LEGACY_INIT
    // The vendor's table, kept verbatim as an escape hatch. It's an ILI9341
    // sequence that the GeeekPi demo shipped for this ST7796 panel — the
    // controller ignores the commands it doesn't know and the shared ones
    // (MADCTL/COLMOD/SLPOUT/DISPON) are enough to bring it up. If the datasheet
    // sequence below ever misbehaves on a panel variant, build with
    // -DST7796_LEGACY_INIT to fall back to what was known to work.
    struct Cmd { uint8_t cmd; uint8_t data[16]; uint8_t n; };
    static const Cmd kInit[] = {
        {0xCF, {0x00, 0x83, 0x30}, 3},
        {0xED, {0x64, 0x03, 0x12, 0x81}, 4},
        {0xE8, {0x85, 0x01, 0x79}, 3},
        {0xCB, {0x39, 0x2C, 0x00, 0x34, 0x02}, 5},
        {0xF7, {0x20}, 1},
        {0xEA, {0x00, 0x00}, 2},
        {0xC0, {0x26}, 1},
        {0xC1, {0x11}, 1},
        {0xC5, {0x35, 0x3E}, 2},
        {0xC7, {0xBE}, 1},
        {0x3A, {0x05}, 1},
        {0xB1, {0x00, 0x1B}, 2},
        {0xF2, {0x08}, 1},
        {0x26, {0x01}, 1},
        {0xE0, {0x1F, 0x1A, 0x18, 0x0A, 0x0F, 0x06, 0x45, 0x87, 0x32, 0x0A, 0x07, 0x02, 0x07, 0x05, 0x00}, 15},
        {0xE1, {0x00, 0x25, 0x27, 0x05, 0x10, 0x09, 0x3A, 0x78, 0x4D, 0x05, 0x18, 0x0D, 0x38, 0x3A, 0x1F}, 15},
        {0xB7, {0x07}, 1},
        {0xB6, {0x0A, 0x82, 0x27, 0x00}, 4},
    };
#else
    // ST7796S datasheet bring-up. 0xF0 is CSCON — the command-set-control gate
    // that unlocks (0xC3/0x96) and re-locks (0x3C/0x69) the manufacturer
    // commands; without it the power/VCOM/gamma writes below are ignored.
    struct Cmd { uint8_t cmd; uint8_t data[16]; uint8_t n; };
    static const Cmd kInit[] = {
        {0xF0, {0xC3}, 1},                    // CSCON: unlock part 1
        {0xF0, {0x96}, 1},                    // CSCON: unlock part 2
        {0x3A, {0x55}, 1},                    // COLMOD: 16 bit/pixel (RGB565)
        {0xB4, {0x01}, 1},                    // DIC: 1-dot inversion
        {0xB6, {0x80, 0x02, 0x3B}, 3},        // DFC: display function control
        {0xB7, {0xC6}, 1},                    // EM: entry mode
        {0xC0, {0x80, 0x45}, 2},              // PWR1
        {0xC1, {0x13}, 1},                    // PWR2: VGH/VGL
        {0xC2, {0xA7}, 1},                    // PWR3
        {0xC5, {0x0A}, 1},                    // VCMPCTL: VCOM
        {0xE8, {0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33}, 8},   // DOCA
        {0xE0, {0xD0, 0x08, 0x0F, 0x06, 0x06, 0x33, 0x30, 0x33,
                0x47, 0x17, 0x13, 0x13, 0x2B, 0x31}, 14},              // PGC
        {0xE1, {0xD0, 0x0A, 0x11, 0x0B, 0x09, 0x07, 0x2F, 0x33,
                0x47, 0x38, 0x15, 0x16, 0x2C, 0x32}, 14},              // NGC
        {0xF0, {0x3C}, 1},                    // CSCON: lock part 1
        {0xF0, {0x69}, 1},                    // CSCON: lock part 2
    };
#endif
    for (const Cmd &c : kInit) {
        cmd(c.cmd);
        if (c.n) data(c.data, c.n);
    }
    hal_delay_ms(120);
}

inline void St7796Module::selfTest() {
    static const uint16_t kBars[] = {
        color::kRed, color::kGreen, color::kBlue, color::kYellow,
        color::kCyan, color::kMagenta, color::kWhite, color::kBlack,
    };
    const int n  = (int)(sizeof(kBars) / sizeof(kBars[0]));
    const int bw = width() / n;
    fill(color::kBlack);
    for (int i = 0; i < n; i++)
        fillRect(i * bw, 0, (i == n - 1) ? width() - i * bw : bw, height() / 2, kBars[i]);
    // A 1px border proves the address window reaches every edge — an off-by-one
    // in CASET/RASET shows up here as a missing line.
    fillRect(0, 0, width(), 1, color::kWhite);
    fillRect(0, height() - 1, width(), 1, color::kWhite);
    fillRect(0, 0, 1, height(), color::kWhite);
    fillRect(width() - 1, 0, 1, height(), color::kWhite);
    int y = height() / 2 + 8;
    drawText(4, y, "ST7796 self test", color::kWhite, color::kBlack, 1); y += 16;
    drawText(4, y, "scale 2 ABCdef 0123", color::kGreen, color::kBlack, 2); y += 24;
    drawText(4, y, "scale 3 XYZ", color::kOrange, color::kBlack, 3);
}

inline void St7796Module::usage(Writer &out) {
    out.writeln("lcd                          panel state");
    out.writeln("lcd on | off                 display on/off (sleep in/out)");
    out.writeln("lcd bl <0-255>               backlight level");
    out.writeln("lcd clear | fill <color>     paint the whole screen");
    out.writeln("lcd rect <x> <y> <w> <h> <color>");
    out.writeln("lcd text <x> <y> <scale> <text…>");
    out.writeln("lcd line <n> <text…>         status line n (scale 2, full width)");
    out.writeln("lcd rotate <0-3>             90° per step");
    out.writeln("lcd invert on|off");
    out.writeln("lcd test                     colour bars + border + text");
    out.writeln("    <color> = name (red, cyan, dim…) or 0xF800 / 63488");
}

inline void St7796Module::info(Writer &out) {
    out.writeln("st7796 panel");
    out.write("  size: ");
    cmdarg::putUInt(out, (uint32_t)width());
    out.write("x");
    cmdarg::putUInt(out, (uint32_t)height());
    out.writeln();
    cmdarg::putField(out, "rotation", _rot);
    cmdarg::putField(out, "spi bus",  _cfg.bus);
    cmdarg::putField(out, "spi hz",   (int32_t)_cfg.hz);
    cmdarg::putField(out, "backlight", _bl);
    cmdarg::putField(out, "display",  _on ? "on" : "off");
    cmdarg::putField(out, "inverted", _cfg.invert ? "yes" : "no");
    out.write("  pins: sck "); cmdarg::putInt(out, _cfg.sck);
    out.write(" mosi ");       cmdarg::putInt(out, _cfg.mosi);
    out.write(" cs ");         cmdarg::putInt(out, _cfg.cs);
    out.write(" dc ");         cmdarg::putInt(out, _cfg.dc);
    out.write(" rst ");        cmdarg::putInt(out, _cfg.rst);
    out.write(" bl ");         cmdarg::putInt(out, _cfg.bl);
    out.writeln();
}

inline void St7796Module::dispatch(const char *args, Writer &out) {
    const char *p = cmdarg::skipSpaces(args);

    if (cmdarg::empty(p) || cmdarg::is(p, "info")) { info(out); return; }
    if (cmdarg::is(p, "help"))                     { usage(out); return; }

    if (cmdarg::is(p, "on") || cmdarg::is(p, "off")) {
        bool on = cmdarg::is(p, "on");
        sleep(!on);
        if (on) backlight(_bl ? _bl : 255);
        out.writeln(on ? "display on" : "display off");
        return;
    }
    if (cmdarg::is(p, "bl")) {
        long v;
        if (!cmdarg::integer(cmdarg::next(p), v, 0, 255)) { usage(out); return; }
        backlight((uint8_t)v);
        out.write("backlight "); cmdarg::putUInt(out, (uint32_t)v); out.writeln();
        return;
    }
    if (cmdarg::is(p, "clear")) { fill(color::kBlack); out.writeln("cleared"); return; }
    if (cmdarg::is(p, "fill")) {
        uint16_t c;
        if (!color::parse(cmdarg::next(p), c)) { out.writeln("unknown colour"); return; }
        fill(c);
        out.writeln("filled");
        return;
    }
    if (cmdarg::is(p, "rect")) {
        long x; long y; long w; long h;
        const char *q = cmdarg::next(p);
        if (!cmdarg::integer(q, x, &q) || !cmdarg::integer(q, y, &q) ||
            !cmdarg::integer(q, w, &q) || !cmdarg::integer(q, h, &q)) { usage(out); return; }
        uint16_t c;
        if (!color::parse(q, c)) { out.writeln("unknown colour"); return; }
        fillRect((int)x, (int)y, (int)w, (int)h, c);
        out.writeln("ok");
        return;
    }
    if (cmdarg::is(p, "text")) {
        long x; long y; long s;
        const char *q = cmdarg::next(p);
        if (!cmdarg::integer(q, x, &q) || !cmdarg::integer(q, y, &q) ||
            !cmdarg::integer(q, s, 1, 8, &q)) { usage(out); return; }
        if (cmdarg::empty(q)) { usage(out); return; }
        drawText((int)x, (int)y, q, color::kWhite, color::kBlack, (int)s);
        out.writeln("ok");
        return;
    }
    if (cmdarg::is(p, "line")) {
        long n;
        const char *q = cmdarg::next(p);
        if (!cmdarg::integer(q, n, 0, 63, &q)) { usage(out); return; }
        statusLine((int)n, cmdarg::empty(q) ? "" : q, color::kWhite, color::kBlack, 2);
        out.writeln("ok");
        return;
    }
    if (cmdarg::is(p, "rotate")) {
        long r;
        if (!cmdarg::integer(cmdarg::next(p), r, 0, 3)) { usage(out); return; }
        setRotation((uint8_t)r);
        fill(color::kBlack);
        out.write("rotation "); cmdarg::putUInt(out, (uint32_t)r);
        out.write(" -> ");      cmdarg::putUInt(out, (uint32_t)width());
        out.write("x");         cmdarg::putUInt(out, (uint32_t)height());
        out.writeln();
        return;
    }
    if (cmdarg::is(p, "invert")) {
        bool on;
        if (!cmdarg::boolean(cmdarg::next(p), on)) { usage(out); return; }
        setInvert(on);
        out.writeln(on ? "inverted" : "normal");
        return;
    }
    if (cmdarg::is(p, "test")) { selfTest(); out.writeln("self test drawn"); return; }

    usage(out);
}

inline void St7796Module::lcdCmd(const char *args, Writer &out, void *ctx) {
    static_cast<St7796Module *>(ctx)->dispatch(args, out);
}

inline void St7796Module::registerCommands(CommandRegistry &reg) {
    reg.registerCommand(CMD(
        "lcd", "TFT panel: on/off, bl, fill, rect, text, line, rotate, test", I2C_NONE,
        lcdCmd, this));
}
