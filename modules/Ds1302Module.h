#pragma once
#include "core/IModule.h"
#include "core/CommandRegistry.h"
#include "hal/hal.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// DS1302 trickle-charge real-time clock — Maxim 3-wire interface (CE / SCLK /
// bidirectional IO), bit-banged over hal_gpio_*. Portable (any platform with
// GPIO). Command `rtc`; the app reads/writes time via the C++ API + the weak
// commander_on_ds1302_ready hook (e.g. seed system time on boot, write back on
// NTP sync). Registers are BCD; values are 24-hour.
struct RtcTime { int year, month, day, hour, minute, second, weekday; };

class Ds1302Module : public IModule {
public:
    Ds1302Module(uint8_t sclk, uint8_t io, uint8_t ce) : _sclk(sclk), _io(io), _ce(ce) {}

    const char *name() const override { return "rtc"; }
    void init() override {
        hal_gpio_set_output(_ce);   hal_gpio_write(_ce, false);
        hal_gpio_set_output(_sclk); hal_gpio_write(_sclk, false);
    }
    void registerCommands(CommandRegistry &reg) override;
    void startTask() override {}

    // ── App API ───────────────────────────────────────────────────────────────
    // Fills t; returns false if the oscillator is halted (CH set → never set/lost
    // power), in which case the time is not valid.
    bool getTime(RtcTime &t) {
        uint8_t s = readReg(0);
        t.second  = bcd2dec(s & 0x7F);
        t.minute  = bcd2dec(readReg(1) & 0x7F);
        t.hour    = bcd2dec(readReg(2) & 0x3F);   // 24-hour
        t.day     = bcd2dec(readReg(3) & 0x3F);
        t.month   = bcd2dec(readReg(4) & 0x1F);
        t.weekday = bcd2dec(readReg(5) & 0x07);
        t.year    = 2000 + bcd2dec(readReg(6));
        return (s & 0x80) == 0;                   // CH bit = clock halted
    }
    void setTime(const RtcTime &t) {
        writeReg(7, 0x00);                          // WP off
        writeReg(0, dec2bcd(t.second) & 0x7F);      // CH = 0 → oscillator runs
        writeReg(1, dec2bcd(t.minute));
        writeReg(2, dec2bcd(t.hour));               // 24-hour (bit 7 = 0)
        writeReg(3, dec2bcd(t.day));
        writeReg(4, dec2bcd(t.month));
        int wd = (t.weekday >= 1 && t.weekday <= 7) ? t.weekday : dow(t.year, t.month, t.day);
        writeReg(5, dec2bcd(wd));
        writeReg(6, dec2bcd(t.year % 100));
        writeReg(7, 0x80);                          // WP on
    }
    bool halted() { return (readReg(0) & 0x80) != 0; }

private:
    uint8_t _sclk, _io, _ce;

    static void delay_us(uint32_t us) {
        uint64_t t0 = hal_time_us();
        while (hal_time_us() - t0 < us) {}
    }
    static uint8_t bcd2dec(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
    static uint8_t dec2bcd(int v)     { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
    // Sakamoto's day-of-week (returns 1=Sun .. 7=Sat).
    static int dow(int y, int m, int d) {
        static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
        if (m < 3) y -= 1;
        int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;  // 0=Sun
        return w + 1;
    }

    void start() { hal_gpio_write(_sclk, false); hal_gpio_write(_ce, true); delay_us(4); }
    void stop()  { hal_gpio_write(_ce, false); delay_us(4); }

    void writeByte(uint8_t v) {
        hal_gpio_set_output(_io);
        for (int i = 0; i < 8; i++) {
            hal_gpio_write(_io, (v >> i) & 1);      // LSB first
            delay_us(1);
            hal_gpio_write(_sclk, true);  delay_us(1);   // DS1302 latches on rising edge
            hal_gpio_write(_sclk, false); delay_us(1);
        }
    }
    uint8_t readByte() {
        uint8_t v = 0;
        hal_gpio_set_input(_io);                    // DS1302 drives IO after the command
        for (int i = 0; i < 8; i++) {
            if (hal_gpio_read(_io)) v |= (1 << i);   // read-then-clock; LSB first
            hal_gpio_write(_sclk, true);  delay_us(1);
            hal_gpio_write(_sclk, false); delay_us(1);   // next bit on falling edge
        }
        return v;
    }
    // reg 0..7 = seconds, minutes, hours, date, month, day-of-week, year, WP.
    uint8_t readReg(uint8_t reg) {
        start(); writeByte(0x81 + reg * 2);
        uint8_t v = readByte(); stop();
        return v;
    }
    void writeReg(uint8_t reg, uint8_t val) {
        start(); writeByte(0x80 + reg * 2); writeByte(val); stop();
    }

    static void rtcCmd(const char *args, Writer &out, void *ctx);
    void        dispatch(const char *args, Writer &out);
    static void putUInt(Writer &out, int v, int width) {
        char b[12]; int n = 0;
        if (v == 0) b[n++] = '0';
        int x = v; while (x) { b[n++] = (char)('0' + x % 10); x /= 10; }
        char s[12]; int i = 0;
        for (int pad = n; pad < width; pad++) s[i++] = '0';
        while (n) s[i++] = b[--n];
        s[i] = '\0'; out.write(s);
    }
};

// Weak app hook — the generated commander_modules.h null-checks and calls this
// after registering. Header-only module, so this is a weak *declaration* (no
// definition to host): if the app doesn't define it, the symbol resolves to null
// and the generated call is skipped; an app-provided strong definition wins.
extern "C" __attribute__((weak)) void commander_on_ds1302_ready(Ds1302Module &);

inline void Ds1302Module::dispatch(const char *args, Writer &out) {
    while (*args == ' ') ++args;
    auto isTok = [](const char *p, const char *t) {
        size_t n = strlen(t);
        return strncmp(p, t, n) == 0 && (p[n] == '\0' || p[n] == ' ');
    };
    const char *p = args;

    if (*p == '\0' || isTok(p, "get")) {
        RtcTime t;
        bool ok = getTime(t);
        static const char *dn[] = {"?", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        putUInt(out, t.year, 4);  out.write("-"); putUInt(out, t.month, 2);
        out.write("-"); putUInt(out, t.day, 2);   out.write(" ");
        putUInt(out, t.hour, 2);  out.write(":"); putUInt(out, t.minute, 2);
        out.write(":"); putUInt(out, t.second, 2);
        out.write(" "); out.write(dn[(t.weekday >= 1 && t.weekday <= 7) ? t.weekday : 0]);
        if (!ok) out.write("  [halted — set the clock]");
        out.writeln();
        return;
    }
    if (isTok(p, "set")) {
        // rtc set YYYY-MM-DD HH:MM:SS
        while (*p && *p != ' ') ++p;   // skip "set"
        while (*p == ' ') ++p;
        char *e;
        RtcTime t;
        t.weekday = 0;
        bool ok = true;
        t.year   = (int)strtol(p, &e, 10); ok = ok && (*e == '-');               p = *e ? e + 1 : e;
        t.month  = (int)strtol(p, &e, 10); ok = ok && (*e == '-');               p = *e ? e + 1 : e;
        t.day    = (int)strtol(p, &e, 10); ok = ok && (*e == ' ' || *e == 'T');  p = *e ? e + 1 : e;
        t.hour   = (int)strtol(p, &e, 10); ok = ok && (*e == ':');               p = *e ? e + 1 : e;
        t.minute = (int)strtol(p, &e, 10); ok = ok && (*e == ':');               p = *e ? e + 1 : e;
        t.second = (int)strtol(p, &e, 10);
        ok = ok && t.year >= 2000 && t.year <= 2099 && t.month >= 1 && t.month <= 12 &&
             t.day >= 1 && t.day <= 31 && t.hour <= 23 && t.minute <= 59 && t.second <= 59;
        if (!ok) { out.writeln("usage: rtc set YYYY-MM-DD HH:MM:SS"); return; }
        setTime(t);
        out.writeln("ok: clock set");
        return;
    }
    if (isTok(p, "dump")) {
        out.write("regs:");
        for (int r = 0; r < 7; r++) { out.write(" "); putUInt(out, readReg(r), 0); }
        out.writeln();
        return;
    }
    out.writeln("rtc            show date/time");
    out.writeln("rtc set YYYY-MM-DD HH:MM:SS");
    out.writeln("rtc dump       raw register values");
}

inline void Ds1302Module::rtcCmd(const char *args, Writer &out, void *ctx) {
    static_cast<Ds1302Module *>(ctx)->dispatch(args, out);
}

inline void Ds1302Module::registerCommands(CommandRegistry &reg) {
    reg.registerCommand(CMD("rtc", "DS1302 real-time clock - 'rtc' / 'rtc set ...'",
                            I2C_NONE, rtcCmd, this));
}
