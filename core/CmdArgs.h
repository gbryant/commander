#pragma once
#include "Writer.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ── Shared command-handler helpers ───────────────────────────────────────────
// Every module that takes arguments ends up writing the same three things: a
// whitespace tokenizer, a keyword match, and printf-free number formatting
// (embedded targets pay real flash for printf, and the AVR tier can't afford it
// at all). Those had been copy-pasted per module; new modules should use these.
//
// Existing modules keep their private copies — this header is additive, not a
// migration. Convert one only when you're editing it anyway.
namespace cmdarg {

// ── Tokenizing ───────────────────────────────────────────────────────────────
inline const char *skipSpaces(const char *p) {
    while (p && (*p == ' ' || *p == '\t')) ++p;
    return p;
}
// Advance past the current token to the start of the next one.
inline const char *next(const char *p) {
    if (!p) return p;
    while (*p && *p != ' ' && *p != '\t') ++p;
    return skipSpaces(p);
}
// True if the token at `p` is exactly `t` (case-sensitive, delimiter-terminated).
inline bool is(const char *p, const char *t) {
    if (!p || !t) return false;
    size_t n = strlen(t);
    return strncmp(p, t, n) == 0 && (p[n] == '\0' || p[n] == ' ' || p[n] == '\t');
}
inline bool empty(const char *p) { return !p || !*p; }

// Parse an integer token (0x hex or decimal, strtol base 0). Returns false and
// leaves `out` untouched if the token isn't a number — callers report usage
// rather than silently acting on a 0 the user never typed.
inline bool integer(const char *p, long &out, const char **rest = nullptr) {
    if (empty(p)) return false;
    char *end;
    long v = strtol(p, &end, 0);
    if (end == p) return false;
    out = v;
    if (rest) *rest = skipSpaces(end);
    return true;
}
// Same, clamped to [lo, hi].
inline bool integer(const char *p, long &out, long lo, long hi, const char **rest = nullptr) {
    long v;
    if (!integer(p, v, rest)) return false;
    out = v < lo ? lo : (v > hi ? hi : v);
    return true;
}
// on/off/toggle style flags: accepts on|off, 1|0, true|false, yes|no.
inline bool boolean(const char *p, bool &out) {
    if (is(p, "on")  || is(p, "1") || is(p, "true")  || is(p, "yes")) { out = true;  return true; }
    if (is(p, "off") || is(p, "0") || is(p, "false") || is(p, "no"))  { out = false; return true; }
    return false;
}

// ── Formatting (no printf) ───────────────────────────────────────────────────
inline void putUInt(Writer &out, uint32_t v) {
    char tmp[11]; int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
    char s[12]; int i = 0;
    while (t) s[i++] = tmp[--t];
    s[i] = '\0';
    out.write(s);
}
inline void putInt(Writer &out, int32_t v) {
    if (v < 0) { out.write("-"); putUInt(out, (uint32_t)(-(int64_t)v)); return; }
    putUInt(out, (uint32_t)v);
}
inline void putHex8(Writer &out, uint8_t v) {
    static const char H[] = "0123456789abcdef";
    char s[3] = {H[(v >> 4) & 0xF], H[v & 0xF], '\0'};
    out.write(s);
}
inline void putHex16(Writer &out, uint16_t v) {
    putHex8(out, (uint8_t)(v >> 8));
    putHex8(out, (uint8_t)(v & 0xFF));
}
// A labelled value on one line: "  label: 42"
inline void putField(Writer &out, const char *label, int32_t v, const char *suffix = nullptr) {
    out.write("  "); out.write(label); out.write(": ");
    putInt(out, v);
    if (suffix) out.write(suffix);
    out.writeln();
}
inline void putField(Writer &out, const char *label, const char *v) {
    out.write("  "); out.write(label); out.write(": "); out.writeln(v);
}

}  // namespace cmdarg
