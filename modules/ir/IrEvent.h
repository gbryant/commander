#pragma once
#include <stdint.h>

// Canonical commander IR event line — the SAME format every platform's IR module emits, so
// the host tools parse one format regardless of board (Arduino IRremote, the Zephyr NEC/Sony
// decoder, etc.). It matches IRremote's `printIRResultShort()` and the regex in the cmdr IR
// tools (`bin/irmap.py` / `bin/irlookup.py`):
//
//     Protocol=<name> Address=0x<addr>, Command=0x<cmd>, Raw-Data=0x<raw>, <bits> bits
//
// The button maps key on (protocol, address, command, bits) — NOT the raw value — so each
// platform must split its decode the protocol's way (NEC: addr=raw&0xff, cmd=(raw>>16)&0xff;
// Sony SIRC: cmd=raw&0x7f, addr=raw>>7). Hand-formatted (no snprintf) to stay cheap on AVR.
// `buf` must hold >= 96 bytes.

inline char *ir_hex(char *p, uint32_t v) {           // minimal-width lowercase hex, no 0x
    if (v == 0) { *p++ = '0'; return p; }
    char tmp[8]; int n = 0;
    while (v) { tmp[n++] = "0123456789abcdef"[v & 0xF]; v >>= 4; }
    while (n) *p++ = tmp[--n];
    return p;
}

inline char *ir_dec(char *p, uint32_t v) {           // minimal-width decimal
    if (v == 0) { *p++ = '0'; return p; }
    char tmp[10]; int n = 0;
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *p++ = tmp[--n];
    return p;
}

inline char *ir_lit(char *p, const char *s) { while (*s) *p++ = *s++; return p; }

inline void ir_format_event(char *buf, const char *proto, uint32_t address,
                            uint32_t command, uint32_t raw, uint16_t bits) {
    char *p = buf;
    p = ir_lit(p, "Protocol=");   p = ir_lit(p, proto);
    p = ir_lit(p, " Address=0x");  p = ir_hex(p, address);
    p = ir_lit(p, ", Command=0x"); p = ir_hex(p, command);
    p = ir_lit(p, ", Raw-Data=0x");p = ir_hex(p, raw);
    p = ir_lit(p, ", ");           p = ir_dec(p, bits);
    p = ir_lit(p, " bits");
    *p = '\0';
}
