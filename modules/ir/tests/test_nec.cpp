// Host test for the portable NEC decoder (modules/ir/NecDecoder.h). Synthesizes edge
// interval streams (with jitter) for known codes and checks decode, repeat, and rejection.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include "modules/ir/NecDecoder.h"
#include "modules/ir/SonyDecoder.h"
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
    {   // canonical event formatter (matches printIRResultShort / the cmdr IR-tool regex)
        char buf[96];
        // Sony 20-bit raw 0x49d15 -> cmd=raw&0x7f=0x15, addr=raw>>7=0x93a (matches sony maps)
        ir_format_event(buf, "Sony", 0x49d15u >> 7, 0x49d15u & 0x7f, 0x49d15u, 20);
        bool ok = (strcmp(buf, "Protocol=Sony Address=0x93a, Command=0x15, Raw-Data=0x49d15, 20 bits") == 0);
        printf("%s ir_format_event Sony -> '%s'\n", ok ? "PASS" : "FAIL", buf); fails += !ok;

        ir_format_event(buf, "NEC", 0x0u, 0x0u, 0x0u, 32);   // zero fields render as 0x0
        bool ok2 = (strcmp(buf, "Protocol=NEC Address=0x0, Command=0x0, Raw-Data=0x0, 32 bits") == 0);
        printf("%s ir_format_event NEC zeros -> '%s'\n", ok2 ? "PASS" : "FAIL", buf); fails += !ok2;
    }
    {   // NEC address split — the maps in cmdr's library are the fixtures, because a mismatch
        // here silently breaks every lookup on one platform while working on another.
        uint32_t addr, cmd;

        // EXTENDED (addr_high is NOT ~addr_low): Hisense Roku, address 0xc7ea in the shipped
        // map. Masking to 8 bits gives 0xea and matches nothing — the bug this test exists for.
        ir_nec_split(0x33c7c7eau, &addr, &cmd);
        bool ok3 = (addr == 0xc7eau && cmd == 0xc7u);
        printf("%s ir_nec_split extended -> addr=0x%x cmd=0x%x (want 0xc7ea/0xc7)\n",
               ok3 ? "PASS" : "FAIL", (unsigned)addr, (unsigned)cmd); fails += !ok3;

        // STANDARD (addr_high == ~addr_low): vizio_sound_bar's address 0x0, whose second byte
        // is 0xff. Taking 16 bits unconditionally would give 0xff00 and break this one instead.
        ir_nec_split(0xbf40ff00u, &addr, &cmd);
        bool ok4 = (addr == 0x0u && cmd == 0x40u);
        printf("%s ir_nec_split standard -> addr=0x%x cmd=0x%x (want 0x0/0x40)\n",
               ok4 ? "PASS" : "FAIL", (unsigned)addr, (unsigned)cmd); fails += !ok4;

        // A standard address whose inverse is easy to eyeball: 0x04 / 0xfb.
        ir_nec_split(0xed12fb04u, &addr, &cmd);
        bool ok5 = (addr == 0x04u && cmd == 0x12u);
        printf("%s ir_nec_split standard 0x04 -> addr=0x%x cmd=0x%x\n",
               ok5 ? "PASS" : "FAIL", (unsigned)addr, (unsigned)cmd); fails += !ok5;
    }
    {   // Sony SIRC: 12-bit code, emitted at the next frame's leading mark
        auto sendSony = [](SonyDecoder &d, uint32_t code, int nbits, int pct) {
            SonyDecoder::Result r = d.feed(jit(2400, pct), true);  // leading mark emits prior frame
            d.feed(jit(600, pct), false);                  // header space
            for (int i = 0; i < nbits; i++) {
                uint32_t bit = (code >> i) & 1;            // LSB first
                d.feed(jit(bit ? 1200 : 600, pct), true);
                d.feed(jit(600, pct), false);
            }
            return r;
        };
        SonyDecoder d;
        sendSony(d, 0x4B2, 12, 0);                          // frame 1 (no emit yet)
        auto r = sendSony(d, 0x4B2, 12, 0);                // frame 2's lead emits frame 1
        bool ok = (r == SonyDecoder::CODE && d.code() == 0x4B2 && d.bits() == 12);
        printf("%s Sony SIRC 12-bit 0x4B2 -> 0x%X (%d bits)\n", ok ? "PASS" : "FAIL", d.code(), d.bits()); fails += !ok;

        // under jitter + the NEC decoder must NOT false-trigger on Sony frames
        SonyDecoder ds; NecDecoder nd;
        bool necQuiet = true;
        auto feedBoth = [&](uint32_t us, bool mark) { if (nd.feed(us, mark) == NecDecoder::CODE) necQuiet = false; ds.feed(us, mark); };
        for (int f = 0; f < 3; f++) {
            feedBoth(2400, true); feedBoth(600, false);
            for (int i = 0; i < 15; i++) { uint32_t b=(0x1234>>i)&1; feedBoth(b?1200:600,true); feedBoth(600,false); }
        }
        printf("%s NEC decoder stays quiet on Sony frames\n", necQuiet ? "PASS" : "FAIL"); fails += !necQuiet;
    }

    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails;
}
