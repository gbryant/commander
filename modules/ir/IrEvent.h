#pragma once
#include <stdint.h>

// Compact, machine-parseable IR event line for a channel/console sink: "0xHHHHHHHH pN"
// (hex code + protocol number). Hand-formatted — no snprintf, so it stays cheap on AVR.
// Shared by every platform IR module so the wire format is identical (the host broker /
// irlookup tooling parses one format regardless of which board produced the press).
// `buf` must hold >= 16 bytes (use 20).
inline void ir_format_event(char *buf, uint32_t code, uint8_t proto) {
    static const char hexd[] = "0123456789ABCDEF";
    char *p = buf;
    *p++ = '0'; *p++ = 'x';
    for (int i = 28; i >= 0; i -= 4) *p++ = hexd[(code >> i) & 0xF];
    *p++ = ' '; *p++ = 'p';
    if (proto >= 100) *p++ = (char)('0' + proto / 100);
    if (proto >= 10)  *p++ = (char)('0' + (proto / 10) % 10);
    *p++ = (char)('0' + proto % 10);
    *p   = '\0';
}
