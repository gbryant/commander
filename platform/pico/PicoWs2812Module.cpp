#include "platform/pico/PicoWs2812Module.h"
#include "core/CmdArgs.h"
#include "hal/hal.h"

#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "ws2812.pio.h"

// PIO plumbing lives here so the header stays free of SDK types (same split as
// PicoIRModule). One state machine drives one chain at 800 kHz.
namespace {
PIO  s_pio    = nullptr;
uint s_sm     = 0;
uint s_offset = 0;
}  // namespace

void PicoWs2812Module::init() {
    // Let the SDK pick a PIO block and state machine — hardcoding pio0/sm0 would
    // collide with the IR module, which also wants a state machine.
    bool ok = pio_claim_free_sm_and_add_program_for_gpio_range(
        &ws2812_program, &s_pio, &s_sm, &s_offset, (uint)_pin, 1, true);
    if (!ok) { _ok = false; return; }
    ws2812_program_init(s_pio, s_sm, s_offset, (uint)_pin, 800000, false);
    _ok = true;
    clear();
}

void PicoWs2812Module::wire(uint8_t r, uint8_t g, uint8_t b, uint8_t *dst) const {
    switch (_order) {
        case RGB: dst[0] = r; dst[1] = g; dst[2] = b; break;
        case BRG: dst[0] = b; dst[1] = r; dst[2] = g; break;
        case RBG: dst[0] = r; dst[1] = b; dst[2] = g; break;
        case GBR: dst[0] = g; dst[1] = b; dst[2] = r; break;
        case BGR: dst[0] = b; dst[1] = g; dst[2] = r; break;
        case GRB:
        default:  dst[0] = g; dst[1] = r; dst[2] = b; break;
    }
}

void PicoWs2812Module::setPixel(int i, uint8_t r, uint8_t g, uint8_t b) {
    if (i < 0 || i >= _count) return;
    wire(r, g, b, &_buf[i * 3]);
}

void PicoWs2812Module::fill(uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < _count; i++) wire(r, g, b, &_buf[i * 3]);
}

void PicoWs2812Module::show() {
    if (!_ok) return;
    for (int i = 0; i < _count; i++) {
        // Brightness is applied here rather than in the buffer, so dimming and
        // re-brightening doesn't quantize the stored colour away.
        uint32_t a = ((uint32_t)_buf[i * 3 + 0] * _bright) / 255;
        uint32_t b = ((uint32_t)_buf[i * 3 + 1] * _bright) / 255;
        uint32_t c = ((uint32_t)_buf[i * 3 + 2] * _bright) / 255;
        // The PIO program shifts out of the MSB, 24 bits per pixel.
        uint32_t word = (a << 24) | (b << 16) | (c << 8);
        pio_sm_put_blocking(s_pio, s_sm, word);
    }
    // WS2812 latches on a >50 µs idle line; give it 300 µs of margin.
    uint64_t t0 = hal_time_us();
    while (hal_time_us() - t0 < 300) {}
}

void PicoWs2812Module::usage(Writer &out) {
    out.writeln("wled <r> <g> <b>          set every pixel (0-255)");
    out.writeln("wled <i> <r> <g> <b>      set pixel i");
    out.writeln("wled off                  all pixels off");
    out.writeln("wled bright <0-255>       brightness applied on show");
    out.writeln("wled test                 cycle red, green, blue");
}

void PicoWs2812Module::dispatch(const char *args, Writer &out) {
    const char *p = cmdarg::skipSpaces(args);

    if (!_ok) { out.writeln("ws2812: no free PIO state machine"); return; }
    if (cmdarg::is(p, "help")) { usage(out); return; }

    if (cmdarg::empty(p)) {
        out.write("ws2812 on gp"); cmdarg::putUInt(out, (uint32_t)_pin);
        out.write(", ");            cmdarg::putUInt(out, (uint32_t)_count);
        out.write(" pixel(s), brightness "); cmdarg::putUInt(out, _bright);
        out.writeln();
        return;
    }
    if (cmdarg::is(p, "off")) { clear(); out.writeln("off"); return; }

    if (cmdarg::is(p, "bright")) {
        long v;
        if (!cmdarg::integer(cmdarg::next(p), v, 0, 255)) { usage(out); return; }
        setBrightness((uint8_t)v);
        show();
        out.write("brightness "); cmdarg::putUInt(out, (uint32_t)v); out.writeln();
        return;
    }
    if (cmdarg::is(p, "test")) {
        const uint8_t rgb[3][3] = {{60, 0, 0}, {0, 60, 0}, {0, 0, 60}};
        for (auto &c : rgb) { fill(c[0], c[1], c[2]); show(); hal_delay_ms(200); }
        clear();
        out.writeln("test done");
        return;
    }

    long a; long b; long c; long d;
    const char *q = p;
    if (!cmdarg::integer(q, a, &q)) { usage(out); return; }
    if (!cmdarg::integer(q, b, &q)) { usage(out); return; }
    if (!cmdarg::integer(q, c, &q)) { usage(out); return; }

    if (cmdarg::integer(q, d, &q)) {            // four numbers = index + colour
        setPixel((int)a, (uint8_t)b, (uint8_t)c, (uint8_t)d);
    } else {                                     // three numbers = whole chain
        fill((uint8_t)a, (uint8_t)b, (uint8_t)c);
    }
    show();
    out.writeln("ok");
}

void PicoWs2812Module::cmd(const char *args, Writer &out, void *ctx) {
    static_cast<PicoWs2812Module *>(ctx)->dispatch(args, out);
}

void PicoWs2812Module::registerCommands(CommandRegistry &reg) {
    reg.registerCommand(CMD(
        "wled", "addressable RGB: set pixels, brightness, test", I2C_NONE, cmd, this));
}
