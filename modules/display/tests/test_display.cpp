// Host tests for the display stack — colour parsing, the 5x7 font, and the
// St7796 driver's wire behaviour (init sequence, address windows, clipping, text
// rasterization) against the recording HAL in tests/fakes. Build via tests/run.sh.
//
// This is the substitute for a panel on the bench: every byte the driver would
// put on SPI is captured and asserted here, so what reaches hardware is a wiring
// question, not a logic one.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "tests/fakes/fake_hal.h"
#include "modules/display/St7796Module.h"
#include "modules/display/St7789Module.h"
#include "core/CommandRegistry.h"

static int fails = 0;
static void check(bool ok, const char *what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

// Pins used by every test below (the Pico Breadboard Kit wiring).
static constexpr int kCS = 5, kDC = 6, kRST = 7;

static St7796Config kitConfig() {
    St7796Config c;
    c.bus = 0; c.sck = 2; c.mosi = 3; c.cs = kCS; c.dc = kDC; c.rst = kRST; c.bl = -1;
    c.nativeW = 320; c.nativeH = 480; c.rotation = 0; c.hz = 40000000; c.invert = true;
    return c;
}

// Commands (DC low) and data (DC high) the driver emitted, in order. The DC pin
// is a parameter because it differs per board — hardcoding it makes the parser
// decode the wrong stream and report nonsense (which it duly did once).
static std::vector<uint8_t> cmds(int dc = kDC)  { return fake_hal::spiWith(dc, false); }
static std::vector<uint8_t> datas(int dc = kDC) { return fake_hal::spiWith(dc, true);  }

static bool sent(uint8_t c, int dc = kDC) {
    for (uint8_t v : cmds(dc)) if (v == c) return true;
    return false;
}

// The last CASET/RASET pair — i.e. the address window of the most recent draw.
struct Window { int x1, x2, y1, y2; bool found; };
static Window lastWindow(int dcPin = kDC) {
    Window w{0, 0, 0, 0, false};
    bool dc = false;
    int  pending = 0;                    // 0x2A or 0x2B awaiting its 4 data bytes
    std::vector<uint8_t> buf;
    for (const auto &e : fake_hal::log) {
        if (e.kind == fake_hal::Event::GpioWrite && e.pin == dcPin) { dc = e.value != 0; continue; }
        if (e.kind != fake_hal::Event::SpiWrite) continue;
        if (!dc) {
            if (e.bytes.size() == 1 && (e.bytes[0] == 0x2A || e.bytes[0] == 0x2B)) {
                pending = e.bytes[0];
                buf.clear();
            } else {
                pending = 0;
            }
        } else if (pending) {
            buf.insert(buf.end(), e.bytes.begin(), e.bytes.end());
            if (buf.size() >= 4) {
                int a = (buf[0] << 8) | buf[1];
                int b = (buf[2] << 8) | buf[3];
                if (pending == 0x2A) { w.x1 = a; w.x2 = b; }
                else                 { w.y1 = a; w.y2 = b; w.found = true; }
                pending = 0;
            }
        }
    }
    return w;
}

// A Writer that captures output, for the command-dispatch tests.
class StringWriter : public Writer {
public:
    void write(const char *s) override   { text += s; }
    void writeln(const char *s) override { text += s; text += "\n"; }
    std::string text;
};

int main() {
    // ── colour parsing ───────────────────────────────────────────────────────
    {
        uint16_t c = 0;
        check(color::parse("red", c)     && c == color::kRed,   "colour: name");
        check(color::parse("CYAN", c)    && c == color::kCyan,  "colour: name is case-insensitive");
        check(color::parse("0xF800", c)  && c == 0xF800,        "colour: 0x hex literal");
        check(color::parse("63488", c)   && c == 63488,         "colour: decimal literal");
        c = 0x1234;
        check(!color::parse("mauve", c)  && c == 0x1234,        "colour: unknown name leaves out untouched");
        check(!color::parse("0x1FFFF", c) && c == 0x1234,       "colour: >16-bit value rejected");
        check(!color::parse("", c),                             "colour: empty string rejected");
        check(rgb565(255, 0, 0) == 0xF800 && rgb565(0, 255, 0) == 0x07E0 &&
              rgb565(0, 0, 255) == 0x001F,                      "rgb565: channel packing");
    }

    // ── font ─────────────────────────────────────────────────────────────────
    {
        check(font5x7::column(' ', 0) == 0 && font5x7::column(' ', 4) == 0, "font: space is blank");
        // '!' is a single centred column: only column 2 has ink.
        check(font5x7::column('!', 2) != 0 && font5x7::column('!', 0) == 0, "font: '!' ink in centre column only");
        // Out-of-range bytes fall back to '?' rather than indexing past the table.
        check(font5x7::column((char)0x01, 2) == font5x7::column('?', 2), "font: control chars render as '?'");
        check(font5x7::column((char)0xFF, 3) == font5x7::column('?', 3), "font: high bytes render as '?'");
        check(font5x7::textWidth("abc", 1) == 18 && font5x7::textWidth("abc", 2) == 36,
              "font: textWidth scales with size");
    }

    // ── init sequence ────────────────────────────────────────────────────────
    {
        fake_hal::reset();
        St7796Module d(kitConfig());
        d.init();
        check(sent(0x11), "init: SLPOUT sent");
        check(sent(0x29), "init: DISPON sent");
        check(sent(0x3A), "init: COLMOD sent");
        check(sent(0x36), "init: MADCTL sent");
        check(sent(0x21), "init: INVON sent (panel ships inverted)");
        check(fake_hal::count(fake_hal::Event::SpiInit) == 1, "init: SPI brought up once");
        check(d.ready(), "init: driver reports ready");

        // COLMOD must be 0x55 (16 bit/pixel) — the whole driver assumes RGB565.
        auto c = cmds(); auto dt = datas();
        bool colmod_ok = false;
        bool dc = false; bool pending = false;
        for (const auto &e : fake_hal::log) {
            if (e.kind == fake_hal::Event::GpioWrite && e.pin == kDC) dc = e.value != 0;
            else if (e.kind == fake_hal::Event::SpiWrite) {
                if (!dc) pending = (e.bytes.size() == 1 && e.bytes[0] == 0x3A);
                else if (pending) { colmod_ok = !e.bytes.empty() && e.bytes[0] == 0x55; pending = false; }
            }
        }
        check(colmod_ok, "init: COLMOD parameter is 0x55 (RGB565)");

        // The reset line must be pulsed low then released before commands flow.
        bool saw_low = false, saw_high_after = false;
        for (const auto &e : fake_hal::log)
            if (e.kind == fake_hal::Event::GpioWrite && e.pin == kRST) {
                if (e.value == 0) saw_low = true;
                else if (saw_low) saw_high_after = true;
            }
        check(saw_low && saw_high_after, "init: hardware reset pulsed low then released");
    }

    // ── geometry and rotation ────────────────────────────────────────────────
    {
        fake_hal::reset();
        St7796Module d(kitConfig());
        d.init();
        check(d.width() == 320 && d.height() == 480, "rotation 0: portrait 320x480");
        d.setRotation(1);
        check(d.width() == 480 && d.height() == 320, "rotation 1: landscape 480x320");
        d.setRotation(2);
        check(d.width() == 320 && d.height() == 480, "rotation 2: portrait again");
        d.setRotation(3);
        check(d.width() == 480 && d.height() == 320, "rotation 3: landscape again");

        // Each rotation must program a different MADCTL value.
        fake_hal::reset();
        std::vector<uint8_t> madctl;
        for (uint8_t r = 0; r < 4; r++) {
            fake_hal::reset();
            d.setRotation(r);
            auto dt = datas();
            madctl.push_back(dt.empty() ? 0 : dt[0]);
        }
        bool distinct = true;
        for (size_t i = 0; i < madctl.size(); i++)
            for (size_t j = i + 1; j < madctl.size(); j++)
                if (madctl[i] == madctl[j]) distinct = false;
        check(distinct, "rotation: each step programs a distinct MADCTL");
    }

    // ── fillRect: address window and pixel count ─────────────────────────────
    {
        fake_hal::reset();
        St7796Module d(kitConfig());
        d.init();

        fake_hal::reset();
        d.fillRect(10, 20, 30, 40, color::kRed);
        Window w = lastWindow();
        check(w.found && w.x1 == 10 && w.x2 == 39 && w.y1 == 20 && w.y2 == 59,
              "fillRect: window is inclusive (x..x+w-1)");
        auto px = fake_hal::pixels();
        check(px.size() == 30 * 40, "fillRect: pushes exactly w*h pixels");
        bool all_red = !px.empty();
        for (uint16_t p : px) if (p != color::kRed) all_red = false;
        check(all_red, "fillRect: every pixel is the requested colour");

        // Full-screen fill.
        fake_hal::reset();
        d.fill(color::kBlue);
        check(fake_hal::pixels().size() == 320u * 480u, "fill: covers the whole panel");
    }

    // ── clipping ─────────────────────────────────────────────────────────────
    {
        fake_hal::reset();
        St7796Module d(kitConfig());
        d.init();

        fake_hal::reset();
        d.fillRect(-10, -10, 20, 20, color::kRed);        // straddles the origin
        Window w = lastWindow();
        check(w.found && w.x1 == 0 && w.y1 == 0 && w.x2 == 9 && w.y2 == 9,
              "clip: negative origin trimmed to 0");
        check(fake_hal::pixels().size() == 100, "clip: only the visible 10x10 is drawn");

        fake_hal::reset();
        d.fillRect(310, 470, 50, 50, color::kRed);        // runs off the far corner
        w = lastWindow();
        check(w.found && w.x2 == 319 && w.y2 == 479, "clip: trimmed to the panel edges");
        check(fake_hal::pixels().size() == 10 * 10, "clip: only the on-screen corner is drawn");

        fake_hal::reset();
        d.fillRect(400, 600, 10, 10, color::kRed);        // entirely off-screen
        check(fake_hal::pixels().empty(), "clip: fully off-screen rect draws nothing");

        fake_hal::reset();
        d.fillRect(10, 10, 0, 50, color::kRed);           // degenerate
        check(fake_hal::pixels().empty(), "clip: zero-width rect draws nothing");
    }

    // ── text ─────────────────────────────────────────────────────────────────
    {
        fake_hal::reset();
        St7796Module d(kitConfig());
        d.init();

        fake_hal::reset();
        int endx = d.drawText(0, 0, "AB", color::kWhite, color::kBlack, 1);
        check(endx == 12, "text: returns x past the last glyph (2 chars * 6px)");
        auto px = fake_hal::pixels();
        check(px.size() == 12u * 8u, "text: pushes the full 12x8 text box");

        // Reconstruct the raster and compare it to the font table directly.
        bool raster_ok = true;
        for (int row = 0; row < 8; row++) {
            for (int col = 0; col < 12; col++) {
                int ci = col / 6, cx = col % 6;
                bool want = (row < 7 && cx < 5) &&
                            ((font5x7::column("AB"[ci], cx) >> row) & 1);
                uint16_t got = px[(size_t)row * 12 + col];
                if (got != (want ? color::kWhite : color::kBlack)) raster_ok = false;
            }
        }
        check(raster_ok, "text: raster matches the font table pixel for pixel");

        // Scale multiplies both axes.
        fake_hal::reset();
        d.drawText(0, 0, "A", color::kWhite, color::kBlack, 3);
        check(fake_hal::pixels().size() == (size_t)(6 * 3) * (8 * 3), "text: scale 3 is 18x24");

        // A run that overflows the right edge is trimmed, not wrapped or dropped.
        fake_hal::reset();
        d.drawText(300, 0, "WIDE TEXT", color::kWhite, color::kBlack, 2);
        Window w = lastWindow();
        check(w.found && w.x2 == 319, "text: long run clipped at the right edge");
        check(!fake_hal::pixels().empty(), "text: clipped run still draws what fits");

        // A negative origin must skip the hidden columns, not program a negative
        // address window (which would hand the panel nonsense).
        fake_hal::reset();
        d.drawText(-6, 0, "AB", color::kWhite, color::kBlack, 1);
        w = lastWindow();
        check(w.found && w.x1 == 0 && w.x2 == 5, "text: negative x clipped to a valid window");
        px = fake_hal::pixels();
        check(px.size() == 6u * 8u, "text: negative x drops exactly the hidden columns");
        {
            // What remains must be the SECOND glyph — the first is off-screen.
            bool right_glyph = true;
            for (int row = 0; row < 8; row++)
                for (int col = 0; col < 6; col++) {
                    bool want = (row < 7 && col < 5) && ((font5x7::column('B', col) >> row) & 1);
                    if (px[(size_t)row * 6 + col] != (want ? color::kWhite : color::kBlack))
                        right_glyph = false;
                }
            check(right_glyph, "text: negative x keeps the correct glyph columns");
        }

        fake_hal::reset();
        d.drawText(0, -4, "A", color::kWhite, color::kBlack, 1);
        w = lastWindow();
        check(w.found && w.y1 == 0 && w.y2 == 3, "text: negative y clipped to a valid window");
        check(fake_hal::pixels().size() == 6u * 4u, "text: negative y drops the hidden rows");

        // Text starting off-screen draws nothing but still reports the end x.
        fake_hal::reset();
        endx = d.drawText(400, 0, "off", color::kWhite, color::kBlack, 1);
        check(fake_hal::pixels().empty() && endx == 418, "text: off-screen origin draws nothing");

        // statusLine clears the full width so a shorter string erases a longer one.
        fake_hal::reset();
        d.statusLine(1, "hi", color::kWhite, color::kBlack, 2);
        check(lastWindow().found, "statusLine: draws");
        size_t total = fake_hal::pixels().size();
        check(total >= 320u * 16u, "statusLine: repaints the full row width");
    }

    // ── blit ─────────────────────────────────────────────────────────────────
    {
        fake_hal::reset();
        St7796Module d(kitConfig());
        d.init();

        uint16_t sprite[4 * 3];
        for (int i = 0; i < 12; i++) sprite[i] = (uint16_t)(1000 + i);

        fake_hal::reset();
        d.blit(5, 5, 4, 3, sprite);
        auto px = fake_hal::pixels();
        check(px.size() == 12, "blit: pushes w*h pixels");
        bool order_ok = true;
        for (int i = 0; i < 12; i++) if (px[(size_t)i] != 1000 + i) order_ok = false;
        check(order_ok, "blit: pixels stream in row-major order");

        // A sprite hanging off the left edge must skip the hidden columns of
        // each source row, not just shift the window.
        fake_hal::reset();
        d.blit(-2, 0, 4, 3, sprite);
        px = fake_hal::pixels();
        check(px.size() == 6, "blit: off-left sprite draws only visible columns");
        check(px.size() == 6 && px[0] == 1002 && px[1] == 1003 &&
              px[2] == 1006 && px[3] == 1007, "blit: correct source columns kept per row");

        fake_hal::reset();
        d.blit(0, 0, 4, 3, nullptr);
        check(fake_hal::pixels().empty(), "blit: null pixel buffer is a no-op");
    }

    // ── shell commands ───────────────────────────────────────────────────────
    {
        fake_hal::reset();
        St7796Module d(kitConfig());
        CommandRegistry reg;
        reg.registerModule(d);

        StringWriter w;
        fake_hal::reset();
        reg.dispatch("lcd rect 0 0 8 8 green", w);
        auto px = fake_hal::pixels();
        check(px.size() == 64, "cmd: 'lcd rect 0 0 8 8 green' draws 64 pixels");
        check(!px.empty() && px[0] == color::kGreen, "cmd: rect uses the named colour");

        w.text.clear(); fake_hal::reset();
        reg.dispatch("lcd rect 0 0 8 8 chartreuse", w);
        check(fake_hal::pixels().empty() && w.text.find("unknown colour") != std::string::npos,
              "cmd: unknown colour reports an error and draws nothing");

        w.text.clear(); fake_hal::reset();
        reg.dispatch("lcd text 4 4 2 hello world", w);
        check(fake_hal::pixels().size() == (size_t)font5x7::textWidth("hello world", 2) * 16,
              "cmd: 'lcd text' keeps spaces in the string");

        w.text.clear(); fake_hal::reset();
        reg.dispatch("lcd bl 128", w);
        check(w.text.find("128") != std::string::npos, "cmd: 'lcd bl' reports the level");

        w.text.clear(); fake_hal::reset();
        reg.dispatch("lcd rotate 1", w);
        check(d.width() == 480 && d.height() == 320, "cmd: 'lcd rotate 1' switches to landscape");
        d.setRotation(0);

        w.text.clear(); fake_hal::reset();
        reg.dispatch("lcd", w);
        check(w.text.find("st7796") != std::string::npos, "cmd: bare 'lcd' prints panel info");

        w.text.clear(); fake_hal::reset();
        reg.dispatch("lcd bogus", w);
        check(w.text.find("lcd test") != std::string::npos, "cmd: unknown subcommand prints usage");

        w.text.clear(); fake_hal::reset();
        reg.dispatch("lcd test", w);
        check(!fake_hal::pixels().empty(), "cmd: 'lcd test' draws the self test");
    }

    // ═══ ST7789 — the window-offset panel ════════════════════════════════════
    // A 240x135 module is a 240x320 controller showing a small window, so every
    // address is shifted, and the shift changes with rotation. Getting this wrong
    // is the classic ST7789 failure: a picture that looks right but is slid a few
    // dozen pixels off the glass, with a band of noise down one edge.
    {
        SpiPanelConfig geek;                    // Waveshare RP2350-GEEK 1.14"
        geek.bus = 0; geek.sck = 10; geek.mosi = 11; geek.cs = 9;
        geek.dc = 8; geek.rst = 12; geek.bl = 25;
        geek.nativeW = 135; geek.nativeH = 240; // the glass, portrait
        geek.ramW = 240;    geek.ramH = 320;    // the controller's RAM
        geek.rotation = 0;  geek.hz = 40000000; geek.invert = true;

        fake_hal::reset();
        St7789Module d(geek);
        d.init();
        const int geekDC = 8;               // this board's DC line, not the kit's
        check(d.ready(), "st7789: init completes");
        check(sent(0x01, geekDC), "st7789: SWRESET sent");
        check(sent(0x11, geekDC) && sent(0x29, geekDC), "st7789: SLPOUT + DISPON sent");
        check(sent(0x21, geekDC), "st7789: INVON sent");
        check(sent(0x3A, geekDC), "st7789: COLMOD sent");
        check(d.width() == 135 && d.height() == 240, "st7789: rotation 0 is 135x240");

        // The offsets, checked one rotation at a time by drawing a single pixel
        // at the origin and reading back the address window.
        struct Expect { uint8_t rot; int ox; int oy; int w; int h; };
        static const Expect kExpect[] = {
            {0, 52, 40, 135, 240},
            {1, 40, 53, 240, 135},
            {2, 53, 40, 135, 240},
            {3, 40, 52, 240, 135},
        };
        bool all_ok = true;
        for (const Expect &e : kExpect) {
            d.setRotation(e.rot);
            check(d.width() == e.w && d.height() == e.h, "st7789: rotation swaps the axes");
            fake_hal::reset();
            d.fillRect(0, 0, 1, 1, color::kRed);
            Window w = lastWindow(geekDC);
            bool ok = w.found && w.x1 == e.ox && w.y1 == e.oy;
            if (!ok) {
                all_ok = false;
                printf("     rot %u: wanted origin %d,%d  got %d,%d\n",
                       e.rot, e.ox, e.oy, w.x1, w.y1);
            }
        }
        check(all_ok, "st7789: origin offset is correct at all four rotations");

        // A full-screen fill must land exactly on the glass — the far edge is
        // what catches an off-by-one in the gap split.
        d.setRotation(0);
        fake_hal::reset();
        d.fill(color::kBlue);
        Window w = lastWindow(geekDC);
        check(w.found && w.x1 == 52 && w.x2 == 186 && w.y1 == 40 && w.y2 == 279,
              "st7789: full-screen fill covers exactly the visible window");
        check(fake_hal::pixels().size() == 135u * 240u, "st7789: full fill pushes 135*240 pixels");

        d.setRotation(1);
        fake_hal::reset();
        d.fill(color::kBlue);
        w = lastWindow(geekDC);
        check(w.found && w.x1 == 40 && w.x2 == 279 && w.y1 == 53 && w.y2 == 187,
              "st7789: landscape fill covers exactly the visible window");

        // Clipping still works in offset space: a rect off the right edge is
        // trimmed to the glass, not to the controller's RAM.
        d.setRotation(0);
        fake_hal::reset();
        d.fillRect(130, 0, 50, 10, color::kRed);
        w = lastWindow(geekDC);
        check(w.found && w.x2 == 52 + 134, "st7789: clipping happens in panel space, then offsets");
        check(fake_hal::pixels().size() == 5u * 10u, "st7789: only the on-glass part is drawn");

        // The manual nudge, for a module whose window isn't centred.
        fake_hal::reset();
        CommandRegistry reg;
        StringWriter sw;
        reg.registerModule(d);                   // re-inits; rotation returns to 0
        sw.text.clear();
        reg.dispatch("lcd offset 2 -3", sw);
        fake_hal::reset();
        d.fillRect(0, 0, 1, 1, color::kRed);
        w = lastWindow(geekDC);
        check(w.found && w.x1 == 54 && w.y1 == 37, "st7789: 'lcd offset' nudges the window");

        sw.text.clear();
        reg.dispatch("lcd", sw);
        check(sw.text.find("st7789") != std::string::npos, "st7789: info names the panel");
        check(sw.text.find("window offset") != std::string::npos, "st7789: info reports the offset");
    }

    // A panel whose RAM matches its glass gets no offset at all — which is why
    // the ST7796 needed no special case.
    {
        fake_hal::reset();
        St7796Module d(kitConfig());
        d.init();
        fake_hal::reset();
        d.fillRect(0, 0, 1, 1, color::kRed);
        Window w = lastWindow();
        check(w.found && w.x1 == 0 && w.y1 == 0, "st7796: no RAM gap means no offset");
    }

    // A 240x240 module (1.3") — gap only on one axis.
    {
        SpiPanelConfig sq;
        sq.nativeW = 240; sq.nativeH = 240; sq.ramW = 240; sq.ramH = 320;
        sq.cs = kCS; sq.dc = kDC; sq.rst = kRST;
        fake_hal::reset();
        St7789Module d(sq);
        d.init();
        fake_hal::reset();
        d.fillRect(0, 0, 1, 1, color::kRed);
        Window w = lastWindow();
        check(w.found && w.x1 == 0 && w.y1 == 40, "st7789 240x240: offset only on the axis with a gap");
    }

    printf("\n%s\n", fails ? "FAILURES" : "all display tests passed");
    return fails ? 1 : 0;
}
