#pragma once
#include <stdint.h>

// Portable Sony SIRC infrared decoder — companion to NecDecoder. Fed the same edge-interval
// stream (`feed(duration_us, wasMark)`), it reconstructs Sony codes (12/15/20-bit, LSB-first
// to match IRremote's decodedRawData). Pure integer math, host-testable.
//
// SIRC frame: a 2.4 ms leading mark, a 0.6 ms space, then N bits — each bit a mark (1.2 ms =
// '1', 0.6 ms = '0') plus a 0.6 ms space — repeated ~every 45 ms while the button is held.
// Spaces are just separators. The frame length isn't on the wire, so we emit a code at the
// next frame boundary (the following 2.4 ms leading mark) once a valid bit count (12/15/20)
// has accumulated — a held/repeated press (Sony auto-repeats) yields one code per frame.
// "mark" = carrier present (TSOP output LOW); "space" = idle (HIGH).
class SonyDecoder {
public:
    enum Result { NONE, CODE };

    Result feed(uint32_t us, bool mark) {
        if (!mark) return NONE;                      // spaces are separators; bits live in marks
        if (within(us, 2400, 25)) {                  // leading mark = frame boundary
            Result r = NONE;
            if (_state == DATA && valid(_bits)) { _code = _data; _nbits = _bits; r = CODE; }
            _state = DATA; _bits = 0; _data = 0;     // start the next frame
            return r;
        }
        if (_state != DATA) { _state = IDLE; return NONE; }
        uint32_t bit;
        if      (within(us, 1200, 30)) bit = 1;
        else if (within(us,  600, 35)) bit = 0;
        else { _state = IDLE; return NONE; }         // not a valid SIRC bit mark
        if (_bits < 20) { _data |= (bit << _bits); _bits++; }   // LSB-first
        return NONE;
    }

    uint32_t code() const { return _code; }
    uint8_t  bits() const { return _nbits; }
    void     reset()       { _state = IDLE; _bits = 0; }

private:
    enum State { IDLE, DATA };
    static bool valid(uint8_t n)  { return n == 12 || n == 15 || n == 20; }
    static bool within(uint32_t v, uint32_t target, uint32_t tol_pct) {
        uint32_t d = target * tol_pct / 100;
        return v >= target - d && v <= target + d;
    }
    State    _state = IDLE;
    uint8_t  _bits  = 0;
    uint8_t  _nbits = 0;
    uint32_t _data  = 0;
    uint32_t _code  = 0;
};
