// Host tests for modules/touch/Gt911Module — the 16-bit register addressing,
// point decoding, the status-register handshake, and the panel→display coordinate
// mapping at every rotation. Run via tests/run.sh.
//
// The mapping is the part worth testing hardest: a touch that lands 90° from
// where the user pressed is the classic symptom, and it's pure arithmetic that a
// laptop can check exactly.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include "tests/fakes/fake_hal.h"
#include "modules/touch/Gt911Module.h"
#include "core/CommandRegistry.h"

static int fails = 0;
static void check(bool ok, const char *what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

class StringWriter : public Writer {
public:
    void write(const char *s) override   { text += s; }
    void writeln(const char *s) override { text += s; text += "\n"; }
    std::string text;
};

// Canned controller state: a 320x480 panel reporting one touch at (x, y).
static void panelReports(int x, int y, bool touching) {
    fake_hal::i2c_regs[0x8140] = {'9', '1', '1', 'x'};                 // product id
    fake_hal::i2c_regs[0x8146] = {0x40, 0x01, 0xE0, 0x01};             // 320 x 480
    fake_hal::i2c_regs[0x814E] = {(uint8_t)(touching ? 0x81 : 0x80)};  // ready + count
    fake_hal::i2c_regs[0x814F] = {
        0x00,                                        // track id
        (uint8_t)(x & 0xFF), (uint8_t)(x >> 8),      // x, little endian
        (uint8_t)(y & 0xFF), (uint8_t)(y >> 8),      // y
        0x10, 0x00,                                  // size
        0x00};
}

int main() {
    // ── init ─────────────────────────────────────────────────────────────────
    {
        fake_hal::reset();
        panelReports(0, 0, false);
        Gt911Module t(0x5D);
        t.init();
        check(t.ready(), "init: reports ready when the controller answers");
        check(strcmp(t.productId(), "911x") == 0, "init: reads the product id");
        check(t.panelWidth() == 320 && t.panelHeight() == 480, "init: reads panel resolution");

        // The address phase must be one combined transaction, not write-then-read.
        bool combined = false;
        for (const auto &e : fake_hal::log)
            if (e.kind == fake_hal::Event::I2cWriteRead && e.bytes.size() == 2 &&
                e.bytes[0] == 0x81 && e.bytes[1] == 0x40) combined = true;
        check(combined, "init: 16-bit register read uses a combined write→read");
        check(fake_hal::count(fake_hal::Event::I2cWrite) == 0,
              "init: never uses the 8-bit-register write path");
    }

    // ── a controller that isn't there ────────────────────────────────────────
    {
        fake_hal::reset();
        fake_hal::i2c_ack = false;
        Gt911Module t(0x5D);
        t.init();
        check(!t.ready(), "init: a silent bus leaves the module not-ready");
        check(t.panelWidth() == 320, "init: falls back to sane panel geometry");

        StringWriter w;
        CommandRegistry reg;
        reg.registerModule(t);
        reg.dispatch("touch", w);
        check(w.text.find("no answer") != std::string::npos,
              "cmd: reports the bus failure instead of a fake coordinate");
    }

    // ── nonsense resolution is rejected ──────────────────────────────────────
    {
        fake_hal::reset();
        panelReports(0, 0, false);
        fake_hal::i2c_regs[0x8146] = {0x00, 0x00, 0xFF, 0xFF};   // 0 x 65535
        Gt911Module t(0x5D);
        t.init();
        check(t.panelWidth() == 320 && t.panelHeight() == 480,
              "init: implausible resolution falls back rather than scaling to garbage");
    }

    // ── reading a point ──────────────────────────────────────────────────────
    {
        fake_hal::reset();
        panelReports(100, 200, true);
        Gt911Module t(0x5D);
        t.init();

        TouchPoint p;
        check(t.read(p), "read: succeeds");
        check(p.pressed, "read: reports pressed");
        check(p.rawX == 100 && p.rawY == 200, "read: decodes little-endian coordinates");
        check(p.x == 100 && p.y == 200, "read: rotation 0 is identity");
        check(p.size == 0x10, "read: decodes touch size");

        // The status register must be cleared after a read, or the controller
        // never publishes another point.
        bool cleared = false;
        for (const auto &e : fake_hal::log)
            if (e.kind == fake_hal::Event::I2cWriteRead && e.bytes.size() == 3 &&
                e.bytes[0] == 0x81 && e.bytes[1] == 0x4E && e.bytes[2] == 0x00) cleared = true;
        check(cleared, "read: clears the status register afterwards");

        // No finger down → released, and the last position is retained.
        fake_hal::i2c_regs[0x814E] = {0x80};      // buffer ready, zero points
        TouchPoint q;
        check(t.read(q) && !q.pressed, "read: no touch reports released");
        check(q.x == 100 && q.y == 200, "read: keeps the last position on release");
    }

    // ── rotation mapping ─────────────────────────────────────────────────────
    {
        fake_hal::reset();
        panelReports(0, 0, true);
        Gt911Module t(0x5D);
        t.init();

        // Corner of the panel: (0,0) must land on a corner at every rotation,
        // and the four rotations must land on four *different* corners.
        int xs[4], ys[4];
        for (uint8_t r = 0; r < 4; r++) {
            t.setRotation(r);
            panelReports(0, 0, true);
            TouchPoint p;
            t.read(p);
            xs[r] = p.x; ys[r] = p.y;
        }
        check(xs[0] == 0   && ys[0] == 0,   "map: rot 0 keeps (0,0) at the origin");
        check(xs[1] == 479 && ys[1] == 0,   "map: rot 1 sends (0,0) to the top-right");
        check(xs[2] == 319 && ys[2] == 479, "map: rot 2 sends (0,0) to the bottom-right");
        check(xs[3] == 0   && ys[3] == 319, "map: rot 3 sends (0,0) to the bottom-left");

        // A mid-panel press stays inside the display bounds at every rotation.
        bool in_bounds = true;
        for (uint8_t r = 0; r < 4; r++) {
            t.setRotation(r);
            panelReports(160, 240, true);
            TouchPoint p;
            t.read(p);
            int w = (r & 1) ? 480 : 320, h = (r & 1) ? 320 : 480;
            if (p.x < 0 || p.x >= w || p.y < 0 || p.y >= h) in_bounds = false;
        }
        check(in_bounds, "map: a centre press stays in bounds at every rotation");

        // flip x mirrors horizontally at rotation 0.
        t.setRotation(0);
        t.setFlip(true, false, false);
        panelReports(10, 20, true);
        TouchPoint p;
        t.read(p);
        check(p.x == 309 && p.y == 20, "map: flip x mirrors horizontally");
        t.setFlip(false, true, false);
        panelReports(10, 20, true);
        t.read(p);
        check(p.x == 10 && p.y == 459, "map: flip y mirrors vertically");
        t.setFlip(false, false, true);
        panelReports(10, 20, true);
        t.read(p);
        check(p.x == 20 && p.y == 10, "map: swap exchanges the axes");
        t.setFlip(false, false, false);
    }

    // ── tick + callback ──────────────────────────────────────────────────────
    {
        fake_hal::reset();
        panelReports(50, 60, false);
        Gt911Module t(0x5D);
        t.init();

        static int calls = 0;
        static TouchPoint seen;
        calls = 0;
        t.onTouch([](const TouchPoint &p, void *) { calls++; seen = p; });

        fake_hal::now_us += 100000;
        panelReports(50, 60, true);
        t.tick();
        check(calls == 1 && seen.pressed && seen.x == 50, "tick: fires the callback on press");

        // Same position again is not a new event.
        fake_hal::now_us += 100000;
        t.tick();
        check(calls == 1, "tick: no callback when nothing changed");

        fake_hal::now_us += 100000;
        panelReports(50, 60, false);
        t.tick();
        check(calls == 2 && !seen.pressed, "tick: fires the callback on release");

        // Polling is rate-limited — a tick right after the last one does nothing.
        int before = (int)fake_hal::count(fake_hal::Event::I2cWriteRead);
        t.tick();
        check((int)fake_hal::count(fake_hal::Event::I2cWriteRead) == before,
              "tick: respects the poll interval");
    }

    // ── shell commands ───────────────────────────────────────────────────────
    {
        fake_hal::reset();
        panelReports(11, 22, true);
        Gt911Module t(0x5D);
        CommandRegistry reg;
        reg.registerModule(t);
        StringWriter w;

        reg.dispatch("touch", w);
        check(w.text.find("11,22") != std::string::npos, "cmd: 'touch' prints the point");

        w.text.clear();
        reg.dispatch("touch raw", w);
        check(w.text.find("raw 11,22") != std::string::npos, "cmd: 'touch raw' prints raw counts");

        w.text.clear();
        reg.dispatch("touch info", w);
        check(w.text.find("320x480") != std::string::npos, "cmd: 'touch info' prints geometry");

        w.text.clear();
        reg.dispatch("touch rotate 3", w);
        check(t.rotation() == 3, "cmd: 'touch rotate' sets the rotation");

        w.text.clear();
        reg.dispatch("touch bogus", w);
        check(w.text.find("touch watch") != std::string::npos, "cmd: unknown subcommand prints usage");

        w.text.clear();
        panelReports(11, 22, false);
        reg.dispatch("touch", w);
        check(w.text.find("no touch") != std::string::npos, "cmd: reports when nothing is pressed");
    }

    printf("\n%s\n", fails ? "FAILURES" : "all touch tests passed");
    return fails ? 1 : 0;
}
