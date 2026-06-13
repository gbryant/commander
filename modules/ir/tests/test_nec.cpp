// Host test for the portable NEC decoder (modules/ir/NecDecoder.h). Synthesizes edge
// interval streams (with jitter) for known codes and checks decode, repeat, and rejection.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include "modules/ir/NecDecoder.h"
#include "modules/ir/IrEvent.h"
#include <cstring>

static int jit(int v, int pct) {            // +/- pct% deterministic-ish jitter
    int d = v * pct / 100;
    return v + (d ? (rand() % (2 * d + 1)) - d : 0);
}

// Drive a full NEC frame for `code` (LSB-first on the wire) into the decoder.
static NecDecoder::Result sendCode(NecDecoder &d, uint32_t code, int pct) {
    NecDecoder::Result r = NecDecoder::NONE;
    r = d.feed(jit(9000, pct), true);       // leading mark
    d.feed(jit(4500, pct), false);          // header space
    for (int i = 0; i < 32; i++) {
        d.feed(jit(560, pct), true);        // bit mark
        uint32_t bit = (code >> i) & 1;     // LSB first
        r = d.feed(jit(bit ? 1690 : 560, pct), false);
    }
    return r;                               // CODE on the 32nd space
}

static NecDecoder::Result sendRepeat(NecDecoder &d) {
    d.feed(9000, true);
    return d.feed(2250, false);
}

int main() {
    int fails = 0;
    srand(1);

    {   // clean decode
        NecDecoder d;
        auto r = sendCode(d, 0x20DF10EF, 0);
        bool ok = (r == NecDecoder::CODE && d.code() == 0x20DF10EF);
        printf("%s clean NEC 0x20DF10EF -> 0x%08X\n", ok ? "PASS" : "FAIL", d.code()); fails += !ok;
    }
    {   // decode under +/-20% jitter, a few codes
        bool ok = true;
        uint32_t codes[] = {0x00FF00FF, 0x12345678, 0xFFFFFFFF, 0x00000001, 0x807F22DD};
        for (uint32_t c : codes) {
            NecDecoder d;
            auto r = sendCode(d, c, 20);
            ok = ok && (r == NecDecoder::CODE && d.code() == c);
        }
        printf("%s 5 codes decode under +/-20%% jitter\n", ok ? "PASS" : "FAIL"); fails += !ok;
    }
    {   // repeat frame
        NecDecoder d;
        auto r = sendRepeat(d);
        bool ok = (r == NecDecoder::REPEAT);
        printf("%s repeat frame -> REPEAT\n", ok ? "PASS" : "FAIL"); fails += !ok;
    }
    {   // garbage spaces never yield a spurious CODE
        NecDecoder d;
        bool spurious = false;
        d.feed(9000, true); d.feed(4500, false);
        for (int i = 0; i < 40; i++) {
            if (d.feed(560, true) == NecDecoder::CODE) spurious = true;
            if (d.feed(1000, false) == NecDecoder::CODE) spurious = true;  // 1000µs = invalid bit
        }
        printf("%s invalid bit widths rejected (no spurious CODE)\n", !spurious ? "PASS" : "FAIL"); fails += spurious;
    }
    {   // event formatter
        char buf[20];
        ir_format_event(buf, 0x20DF10EF, 3);
        bool ok = (strcmp(buf, "0x20DF10EF p3") == 0);
        printf("%s ir_format_event -> '%s'\n", ok ? "PASS" : "FAIL", buf); fails += !ok;
    }

    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails;
}
