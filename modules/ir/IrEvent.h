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
// platform must split its decode the protocol's way: NEC via `ir_nec_split()` below, Sony SIRC
// as cmd=raw&0x7f, addr=raw>>7. Hand-formatted (no snprintf) to stay cheap on AVR.
// `buf` must hold >= 96 bytes.

// NEC address/command split, IRremote-compatible — use this rather than hand-masking, or maps
// built on one board won't match presses decoded on another.
//
// Bit k of raw is the k-th bit received, so:
//   bits[7:0] = addr_low (1st byte)   bits[15:8]  = addr_high (2nd)
//   bits[23:16] = command (3rd)       bits[31:24] = ~command  (4th)
//
// The catch is the address. In *standard* NEC the 2nd byte is the bitwise inverse of the 1st
// (redundancy), and the address is 8 bits. In *extended* NEC that redundancy is traded for
// address space: the two bytes are independent and the address is the full 16. IRremote picks
// between them by testing the inversion, and so must we — masking to 8 bits unconditionally
// truncates every extended remote (a Hisense Roku's 0xc7ea decodes as 0xea and matches
// nothing), while taking 16 unconditionally corrupts every standard one (0x0 becomes 0xff00).
inline void ir_nec_split(uint32_t raw, uint32_t *address, uint32_t *command) {
    uint8_t addr_low  = (uint8_t)(raw & 0xFF);
    uint8_t addr_high = (uint8_t)((raw >> 8) & 0xFF);
    *command = (raw >> 16) & 0xFF;
    *address = (addr_high == (uint8_t)~addr_low)
                   ? addr_low                                        // standard NEC: 8-bit
                   : (uint32_t)(((uint32_t)addr_high << 8) | addr_low);  // extended: 16-bit
}

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
