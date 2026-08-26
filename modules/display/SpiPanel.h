#pragma once
#include "core/IModule.h"
#include "core/CommandRegistry.h"
#include "core/CmdArgs.h"
#include "hal/hal.h"
#include "modules/display/IDisplay.h"
#include "modules/display/Font5x7.h"
#include <string.h>

// ── Shared driver for MIPI-DCS-style SPI panels ──────────────────────────────
// ST7789, ST7796, ILI9341 and friends differ in their power/gamma bring-up and
// their MADCTL values, but they all speak the same drawing protocol: CASET,
// RASET, RAMWR, 16-bit pixels. This class is that protocol, plus the pieces
// every panel needs anyway — clipping, a row buffer, text, the `lcd` command
// surface — and a panel subclass supplies only what genuinely differs:
//
//     sendInitSequence()   the power-on register writes
//     madctl(rotation)     the memory-access value per 90° step
//
// Portable: hal_spi_* + hal_gpio_* + hal_pwm_* only.
//
// **Controller RAM vs panel size.** A controller's RAM is often larger than the
// glass in front of it — a 240x135 ST7789 module is a 240x320 controller showing
// a small window — so every address must be shifted by an offset that changes
// with rotation. Rather than a per-panel table of magic numbers, the offset is
// derived from the gap between `ramW/ramH` and `nativeW/nativeH`, split across
// the two edges (and swapped for the odd rotations, since the axes swap with
// the memory). For a panel whose RAM matches its glass the gap is zero and every
// offset is zero, which is why the ST7796 needs no special case. Panels that
// don't centre their window can be corrected live with `lcd offset <x> <y>`.
//
// Memory: one row buffer of kMaxRow pixels. There is no framebuffer — a 320x480
// panel would need 300 KB — so drawing goes straight out the wire and the panel
// holds the only copy. That's also why there's no read-back or scroll.

struct SpiPanelConfig {
    uint8_t  bus      = 0;      // SPI controller (the pins decide which; 0 = spi0)
    int8_t   sck      = 2;
    int8_t   mosi     = 3;
    int8_t   cs       = 5;
    int8_t   dc       = 6;
    int8_t   rst      = 7;
    int8_t   bl       = -1;     // PWM backlight pin, -1 = hard-wired on
    uint16_t nativeW  = 320;    // the glass, in its rotation-0 orientation
    uint16_t nativeH  = 480;
    uint16_t ramW     = 0;      // controller RAM; 0 = same as the glass
    uint16_t ramH     = 0;
    uint8_t  rotation = 0;      // 0..3, 90° per step
    uint32_t hz       = 40000000;
    bool     invert   = true;   // most of these panels want INVON
};

class SpiPanel : public IModule, public IDisplay {
public:
    explicit SpiPanel(const SpiPanelConfig &cfg) : _cfg(cfg), _rot(cfg.rotation & 3) {
        if (_cfg.ramW == 0) _cfg.ramW = _cfg.nativeW;
        if (_cfg.ramH == 0) _cfg.ramH = _cfg.nativeH;
    }

    const char *name() const override { return "lcd"; }

    void init() override {
        if (_cfg.cs  >= 0) { hal_gpio_set_output((uint8_t)_cfg.cs);  hal_gpio_write((uint8_t)_cfg.cs, true); }
        if (_cfg.dc  >= 0) { hal_gpio_set_output((uint8_t)_cfg.dc);  hal_gpio_write((uint8_t)_cfg.dc, true); }
        if (_cfg.rst >= 0)   hal_gpio_set_output((uint8_t)_cfg.rst);
        if (_cfg.bl  >= 0)   hal_pwm_init((uint8_t)_cfg.bl);

        hal_spi_init(_cfg.bus, _cfg.sck, _cfg.mosi, -1, _cfg.hz);
        hardwareReset();
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

    void registerCommands(CommandRegistry &reg) override {
        reg.registerCommand(CMD(
            "lcd", "TFT panel: on/off, bl, fill, rect, text, line, rotate, test", I2C_NONE,
            lcdCmd, this));
    }

    // ── IDisplay ─────────────────────────────────────────────────────────────
    int width()  const override { return (_rot & 1) ? _cfg.nativeH : _cfg.nativeW; }
    int height() const override { return (_rot & 1) ? _cfg.nativeW : _cfg.nativeH; }

    void fill(uint16_t c) override { fillRect(0, 0, width(), height(), c); }

    void fillRect(int x, int y, int w, int h, uint16_t c) override {
        if (!clip(x, y, w, h)) return;
        setWindow(x, y, w, h);
        // The row buffer bounds one push, not the rectangle: a panel wider than
        // kMaxRow still fills correctly, in chunks.
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
        // Clipping a blit means skipping rows/columns of the source, so it's done
        // by hand rather than through clip() — a partially off-screen sprite is a
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
        // A negative origin can't just be clamped — the address window would go
        // negative and the panel would be handed nonsense — so the hidden
        // rows/columns are skipped in the glyph lookup instead, exactly as blit()
        // skips hidden source pixels.
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
        // No backlight pin: fall back to the panel's own display on/off so
        // `lcd bl 0` still visibly does something.
        bool want = level > 0;
        if (want != _on) { cmd(want ? 0x29 : 0x28); _on = want; }
    }

    // ── Panel-level extras (not in IDisplay) ─────────────────────────────────
    bool ready() const { return _ready; }
    uint8_t rotation() const { return _rot; }

    void setRotation(uint8_t r) {
        _rot = r & 3;
        cmd(0x36);                                  // MADCTL
        uint8_t v = madctl(_rot);
        data(&v, 1);
    }
    void setInvert(bool on) { _cfg.invert = on; cmd(on ? 0x21 : 0x20); }
    void sleep(bool on) {
        cmd(on ? 0x10 : 0x11);
        hal_delay_ms(on ? 5 : 120);
        _on = !on;
    }
    // Manual override of the derived window offset, for a panel variant whose
    // visible area isn't where the gap arithmetic puts it. `lcd offset x y`.
    void setOffsetAdjust(int dx, int dy) { _adjX = dx; _adjY = dy; }

    // A visible end-to-end check: colour bars, a 1px border (which proves the
    // address window reaches every edge — the offsets' acid test) and text at
    // three scales. The first thing to run when the hardware lands.
    void selfTest() {
        static const uint16_t kBars[] = {
            color::kRed, color::kGreen, color::kBlue, color::kYellow,
            color::kCyan, color::kMagenta, color::kWhite, color::kBlack,
        };
        const int n  = (int)(sizeof(kBars) / sizeof(kBars[0]));
        const int bw = width() / n;
        fill(color::kBlack);
        for (int i = 0; i < n; i++)
            fillRect(i * bw, 0, (i == n - 1) ? width() - i * bw : bw, height() / 2, kBars[i]);
        fillRect(0, 0, width(), 1, color::kWhite);
        fillRect(0, height() - 1, width(), 1, color::kWhite);
        fillRect(0, 0, 1, height(), color::kWhite);
        fillRect(width() - 1, 0, 1, height(), color::kWhite);
        int y = height() / 2 + 4;
        drawText(4, y, panelName(), color::kWhite, color::kBlack, 1);  y += 12;
        drawText(4, y, "scale 2 ABCdef 0123", color::kGreen, color::kBlack, 2);
        if (height() > 200) {
            y += 24;
            drawText(4, y, "scale 3 XYZ", color::kOrange, color::kBlack, 3);
        }
    }

protected:
    static constexpr int kMaxRow = 480;   // longest panel edge we buffer a row of

    // ── What a panel subclass provides ───────────────────────────────────────
    virtual void        sendInitSequence()          = 0;
    virtual uint8_t     madctl(uint8_t rot) const   = 0;
    virtual const char *panelName() const           = 0;

    // Send one command / one data run. Available to subclasses for their init.
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
    // Run a {cmd, data…} table — the shape every panel's bring-up takes.
    struct InitCmd { uint8_t cmd; uint8_t data[16]; uint8_t n; };
    void runInit(const InitCmd *seq, size_t count) {
        for (size_t i = 0; i < count; i++) {
            cmd(seq[i].cmd);
            if (seq[i].n) data(seq[i].data, seq[i].n);
        }
        hal_delay_ms(120);
    }

    SpiPanelConfig _cfg;

private:
    uint8_t  _rot   = 0;
    uint8_t  _bl    = 255;
    bool     _on    = false;
    bool     _ready = false;
    int      _adjX  = 0, _adjY = 0;
    uint16_t _row[kMaxRow] = {};

    void select(bool on)  { if (_cfg.cs >= 0) hal_gpio_write((uint8_t)_cfg.cs, !on); }
    void dcCommand()      { if (_cfg.dc >= 0) hal_gpio_write((uint8_t)_cfg.dc, false); }
    void dcData()         { if (_cfg.dc >= 0) hal_gpio_write((uint8_t)_cfg.dc, true); }

    // Pixel pushes keep CS asserted across the whole window write (endWrite()
    // releases it) — releasing per row would cost a GPIO round trip per row.
    void pushPixels(const uint16_t *px, int count) {
        dcData(); select(true);
        hal_spi_write16(_cfg.bus, px, (size_t)count);
    }
    void endWrite() { select(false); }

    void hardwareReset() {
        if (_cfg.rst < 0) return;
        hal_gpio_write((uint8_t)_cfg.rst, true);  hal_delay_ms(20);
        hal_gpio_write((uint8_t)_cfg.rst, false); hal_delay_ms(20);
        hal_gpio_write((uint8_t)_cfg.rst, true);  hal_delay_ms(120);
    }

    // Where the visible window sits in controller RAM, for the current rotation.
    // The gap is split across the two edges; the odd rotations swap the axes
    // because the memory rotates with them.
    void windowOffset(int &ox, int &oy) const {
        int gapX = (int)_cfg.ramW - (int)_cfg.nativeW;
        int gapY = (int)_cfg.ramH - (int)_cfg.nativeH;
        if (gapX < 0) gapX = 0;
        if (gapY < 0) gapY = 0;
        switch (_rot) {
            case 0:  ox = gapX / 2;          oy = gapY / 2;          break;
            case 1:  ox = gapY / 2;          oy = gapX - gapX / 2;   break;
            case 2:  ox = gapX - gapX / 2;   oy = gapY - gapY / 2;   break;
            default: ox = gapY - gapY / 2;   oy = gapX / 2;          break;
        }
        ox += _adjX;
        oy += _adjY;
    }

    void setWindow(int x, int y, int w, int h) {
        int ox, oy;
        windowOffset(ox, oy);
        x += ox; y += oy;
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

    // ── Shell ────────────────────────────────────────────────────────────────
    static void lcdCmd(const char *args, Writer &out, void *ctx) {
        static_cast<SpiPanel *>(ctx)->dispatch(args, out);
    }
    void dispatch(const char *args, Writer &out);
    void usage(Writer &out);
    void info(Writer &out);
};

// Weak app hook — the generated commander_modules.h null-checks and calls this
// after registering. It hands over the IDisplay interface rather than the
// concrete driver, so app screens don't bind to one panel.
extern "C" void commander_on_display_ready(IDisplay &) __attribute__((weak));

// ─────────────────────────────────────────────────────────────────────────────

inline void SpiPanel::usage(Writer &out) {
    out.writeln("lcd                          panel state");
    out.writeln("lcd on | off                 display on/off (sleep in/out)");
    out.writeln("lcd bl <0-255>               backlight level");
    out.writeln("lcd clear | fill <color>     paint the whole screen");
    out.writeln("lcd rect <x> <y> <w> <h> <color>");
    out.writeln("lcd text <x> <y> <scale> <text…>");
    out.writeln("lcd line <n> <text…>         status line n (scale 2, full width)");
    out.writeln("lcd rotate <0-3>             90° per step");
    out.writeln("lcd invert on|off");
    out.writeln("lcd offset <dx> <dy>         nudge the window (panel variants)");
    out.writeln("lcd test                     colour bars + border + text");
    out.writeln("    <color> = name (red, cyan, dim…) or 0xF800 / 63488");
}

inline void SpiPanel::info(Writer &out) {
    out.write(panelName()); out.writeln(" panel");
    out.write("  size: ");
    cmdarg::putUInt(out, (uint32_t)width());
    out.write("x");
    cmdarg::putUInt(out, (uint32_t)height());
    out.writeln();
    cmdarg::putField(out, "rotation", _rot);
    int ox, oy;
    windowOffset(ox, oy);
    out.write("  window offset: "); cmdarg::putInt(out, ox);
    out.write(",");                 cmdarg::putInt(out, oy);
    if (_adjX || _adjY) {
        out.write("  (adjust ");  cmdarg::putInt(out, _adjX);
        out.write(",");           cmdarg::putInt(out, _adjY);
        out.write(")");
    }
    out.writeln();
    cmdarg::putField(out, "spi bus",   _cfg.bus);
    cmdarg::putField(out, "spi hz",    (int32_t)_cfg.hz);
    cmdarg::putField(out, "backlight", _bl);
    cmdarg::putField(out, "display",   _on ? "on" : "off");
    cmdarg::putField(out, "inverted",  _cfg.invert ? "yes" : "no");
    out.write("  pins: sck "); cmdarg::putInt(out, _cfg.sck);
    out.write(" mosi ");       cmdarg::putInt(out, _cfg.mosi);
    out.write(" cs ");         cmdarg::putInt(out, _cfg.cs);
    out.write(" dc ");         cmdarg::putInt(out, _cfg.dc);
    out.write(" rst ");        cmdarg::putInt(out, _cfg.rst);
    out.write(" bl ");         cmdarg::putInt(out, _cfg.bl);
    out.writeln();
}

inline void SpiPanel::dispatch(const char *args, Writer &out) {
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
    if (cmdarg::is(p, "offset")) {
        long dx; long dy;
        const char *q = cmdarg::next(p);
        if (!cmdarg::integer(q, dx, &q) || !cmdarg::integer(q, dy, &q)) { usage(out); return; }
        setOffsetAdjust((int)dx, (int)dy);
        fill(color::kBlack);
        selfTest();
        out.write("offset adjust "); cmdarg::putInt(out, (int32_t)dx);
        out.write(",");              cmdarg::putInt(out, (int32_t)dy);
        out.writeln(" — check the border reaches all four edges");
        return;
    }
    if (cmdarg::is(p, "test")) { selfTest(); out.writeln("self test drawn"); return; }

    usage(out);
}
