#pragma once
#include <stdint.h>

// Portable NEC infrared decoder. Fed the stream of edge intervals (`feed(duration_us,
// wasMark)`) — e.g. from a GPIO edge interrupt timestamping with a cycle counter — it
// reconstructs 32-bit NEC codes and repeat frames. Pure integer math, zero platform deps,
// so it's host-testable and shared by any platform that can produce edge timings (the
// Zephyr GPIO-ISR backend uses it; AVR/Pico/ESP32 keep their hardware decoders).
//
// NEC frame: a 9 ms leading mark + 4.5 ms space, then 32 bits (each a ~560 µs mark + a
// space: ~560 µs = 0, ~1690 µs = 1), LSB-first — so the assembled 32-bit value matches
// IRremote's decodedRawData. A held button repeats a 9 ms mark + 2.25 ms space frame.
// "mark" = carrier present (a TSOP receiver drives its output LOW); "space" = idle (HIGH).
class NecDecoder {
public:
    enum Result { NONE, CODE, REPEAT };

    Result feed(uint32_t us, bool mark) {
        if (mark) {
            if (within(us, 9000, 25)) { _state = HDR; return NONE; }   // leading mark
            // Otherwise it should be a ~560 µs bit/lead mark while we're mid-frame;
            // anything wildly off aborts and waits for the next header.
            if (_state == HDR || _state == DATA) {
                if (!within(us, 560, 50)) _state = IDLE;
            } else {
                _state = IDLE;
            }
            return NONE;
        }
        switch (_state) {                                              // spaces
        case HDR:
            if (within(us, 4500, 25)) { _state = DATA; _bits = 0; _data = 0; return NONE; }
            if (within(us, 2250, 30)) { _state = IDLE; return REPEAT; }
            _state = IDLE; return NONE;
        case DATA: {
            uint32_t bit;
            if      (within(us, 1690, 30)) bit = 1;
            else if (within(us,  560, 50)) bit = 0;
            else { _state = IDLE; return NONE; }                       // not a valid 0/1
            _data = (_data >> 1) | (bit << 31);                        // NEC is LSB-first
            if (++_bits == 32) { _state = IDLE; _code = _data; return CODE; }
            return NONE;
        }
        default:
            return NONE;
        }
    }

    uint32_t code() const { return _code; }
    void     reset()       { _state = IDLE; _bits = 0; }

private:
    enum State { IDLE, HDR, DATA };
    static bool within(uint32_t v, uint32_t target, uint32_t tol_pct) {
        uint32_t d = target * tol_pct / 100;
        return v >= target - d && v <= target + d;
    }
    State    _state = IDLE;
    uint8_t  _bits  = 0;
    uint32_t _data  = 0;
    uint32_t _code  = 0;
};
