#pragma once
#include <stdint.h>
#include <stddef.h>

// COBS-framed channel bus codec — the wire format for the commander channel layer
// (peer pub/sub between an MCU and a host/SBC; see docs/commander-channels-design.md).
//
// Each frame on the wire is:  COBS( [channel] [payload...] )  +  0x00 delimiter.
// COBS (Consistent Overhead Byte Stuffing) makes the encoded body contain no 0x00, so
// 0x00 is an unambiguous frame boundary — the stream is self-synchronizing (a reader
// recovers at the next delimiter after corruption/overflow). Pure + host-testable; no
// platform deps. The channel-mux transport layers on top of this.

#ifndef CMDR_CH_FRAME_MAX
#define CMDR_CH_FRAME_MAX 256   // max decoded frame ([channel]+payload); override as needed
#endif

static constexpr uint8_t CH_DELIM = 0x00;

// ── COBS ──────────────────────────────────────────────────────────────────────
// Encode `len` bytes (src) into dst (needs up to len + len/254 + 1). Returns encoded
// length, excluding the trailing delimiter (the framer appends that).
inline size_t cobs_encode(const uint8_t *src, size_t len, uint8_t *dst) {
    size_t wr = 0;
    size_t code_pos = wr++;     // reserve the first code byte
    uint8_t code = 1;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == 0) {
            dst[code_pos] = code; code_pos = wr++; code = 1;
        } else {
            dst[wr++] = src[i];
            if (++code == 0xFF) { dst[code_pos] = code; code_pos = wr++; code = 1; }
        }
    }
    dst[code_pos] = code;
    return wr;
}

// Decode `len` COBS bytes (no delimiter) into dst. Returns decoded length, 0 if malformed.
inline size_t cobs_decode(const uint8_t *src, size_t len, uint8_t *dst) {
    size_t rd = 0, wr = 0;
    while (rd < len) {
        uint8_t code = src[rd++];
        if (code == 0) return 0;                 // no zeros allowed inside COBS data
        for (uint8_t i = 1; i < code; i++) {
            if (rd >= len) return 0;              // truncated run
            dst[wr++] = src[rd++];
        }
        if (code != 0xFF && rd < len) dst[wr++] = 0;  // implicit zero (not after a full run / at end)
    }
    return wr;
}

// ── Channel frame ───────────────────────────────────────────────────────────────
// Encode one frame: COBS([channel|payload]) + delimiter, into `out`
// (size ≥ len + (len+1)/254 + 3). Returns total bytes written.
inline size_t channel_encode(uint8_t channel, const uint8_t *payload, size_t len, uint8_t *out) {
    size_t wr = 0;
    size_t code_pos = wr++;
    uint8_t code = 1;
    for (size_t i = 0; i <= len; i++) {          // i==0 → channel, then payload
        uint8_t b = (i == 0) ? channel : payload[i - 1];
        if (b == 0) {
            out[code_pos] = code; code_pos = wr++; code = 1;
        } else {
            out[wr++] = b;
            if (++code == 0xFF) { out[code_pos] = code; code_pos = wr++; code = 1; }
        }
    }
    out[code_pos] = code;
    out[wr++] = CH_DELIM;
    return wr;
}

// Streaming frame reader: feed bytes; returns true when a complete frame is decoded,
// then channel()/payload()/len() are valid until the next feed().
class ChannelReader {
public:
    bool feed(uint8_t b) {
        if (b == CH_DELIM) {
            if (_n == 0) return false;            // empty/resync gap
            size_t dl = cobs_decode(_raw, _n, _decoded);
            _n = 0;
            if (dl == 0) return false;            // malformed → drop
            _channel = _decoded[0];
            _len     = dl - 1;
            return true;
        }
        if (_n < sizeof(_raw)) _raw[_n++] = b;
        else                   _n = 0;            // overflow → drop, resync at next delimiter
        return false;
    }
    uint8_t        channel() const { return _channel; }
    const uint8_t *payload() const { return _decoded + 1; }
    size_t         len()     const { return _len; }

private:
    uint8_t  _raw[CMDR_CH_FRAME_MAX + (CMDR_CH_FRAME_MAX / 254) + 2];  // COBS-encoded in-flight
    uint8_t  _decoded[CMDR_CH_FRAME_MAX];
    size_t   _n = 0;
    uint8_t  _channel = 0;
    size_t   _len = 0;
};
