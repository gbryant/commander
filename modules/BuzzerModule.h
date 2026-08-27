#pragma once
#include "core/IModule.h"
#include "core/CommandRegistry.h"
#include "core/CmdArgs.h"
#include "hal/hal.h"
#include <stdint.h>
#include <string.h>

// ── Piezo buzzer / small speaker on a PWM pin ────────────────────────────────
// Portable: hal_pwm_tone() only. Any platform whose HAL implements PWM gets it.
//
// Everything is non-blocking. Tones and melodies are driven from tick(), so
// `buzz play c4:200,e4:200,g4:400` returns immediately and the shell stays live
// while it plays — the original kit code bit-banged the pin in a busy loop with
// interrupts running, which blocks whatever task it lands in.
//
// Note syntax: <note><octave>[#|b]:<ms>, comma-separated. `r` is a rest.
//   c4:200,e4:200,g4:400      an arpeggio
//   a4:100,r:50,a4:100        two beeps
// Frequencies come from an integer table shifted by octave — no libm, no floats.

class BuzzerModule : public IModule {
public:
    explicit BuzzerModule(uint8_t pin) : _pin(pin) {}

    const char *name() const override { return "buzz"; }

    void init() override {
        hal_pwm_init(_pin);
        hal_pwm_stop(_pin);
    }

    void registerCommands(CommandRegistry &reg) override;

    /*
     * Every mode is checked on every tick — deliberately.
     *
     * The previous version returned immediately unless a timed note was
     * playing, which meant the pin could be left oscillating in a state nothing
     * ever looked at again: an actuator stuck on, with no way back except a
     * reboot. That happened on hardware (a buzzer stuck in a continuous tone,
     * from both the touch and button paths).
     *
     * So `Off` now means silent and is *verified* rather than assumed, and even
     * an "until stop()" tone carries a safety cap. A buzzer that can get stuck
     * on is worse than one that occasionally cuts a long note short.
     */
    void tick() override {
        uint64_t now = hal_time_us();
        switch (_mode) {
            case Off:
                if (_current != 0) {          // sounding with nothing scheduled
                    _lostStops++;
                    stop();
                }
                return;
            case Timed:
                if (now >= _noteEnd) stop();
                return;
            case Sequence:
                if (now >= _noteEnd) advance();
                return;
            case Indefinite:
                if (now >= _noteEnd) { _lostStops++; stop(); }
                return;
        }
    }

    // ── App API ──────────────────────────────────────────────────────────────
    // A single tone for `ms` (0 = until stop()).
    // Every path that makes noise goes through here, so volume (and mute) can't
    // be bypassed by one of them.
    void sound(uint32_t hz) {
        if (hz == 0 || _volume == 0) { hal_pwm_stop(_pin); return; }
        hal_pwm_tone(_pin, hz, dutyForVolume());
    }
    void reapply() {                       // volume changed mid-note
        if (_mode == Off) return;
        sound(_current);
    }
    uint8_t dutyForVolume() const {
        // 50% duty is the loudest a square wave drives a piezo; scale to that.
        uint32_t d = ((uint32_t)_volume * 50u) / 100u;
        return (uint8_t)(d == 0 ? 1 : d);
    }

    void tone(uint32_t hz, uint32_t ms = 0) {
        _seq[0] = '\0';
        _seqPos = 0;
        if (hz == 0) { stop(); return; }
        sound(hz);
        _current  = hz;
        _toneCalls++;
        _lastMs   = ms;
        uint64_t now = hal_time_us();
        if (ms) {
            _noteEnd = now + (uint64_t)ms * 1000;
            _mode    = Timed;
        } else {
            // "Until stop()" still gets an end time — a safety cap, not a
            // duration. See the watchdog note on tick().
            _noteEnd = now + kMaxSoundUs;
            _mode    = Indefinite;
        }
    }

    void stop() {
        hal_pwm_stop(_pin);
        _mode    = Off;
        _current = 0;
        _seq[0]  = '\0';
        _seqPos  = 0;
        _stopCalls++;
    }

    void beep() { tone(2000, 60); }

    // Queue a comma-separated note sequence. Replaces anything already playing.
    bool play(const char *seq) {
        if (!seq || !*seq) return false;
        strncpy(_seq, seq, kSeqMax - 1);
        _seq[kSeqMax - 1] = '\0';
        _seqPos  = 0;
        _noteEnd = 0;
        _mode    = Sequence;
        advance();
        return true;
    }

    // Short built-in patterns, so an app can say something happened without
    // inventing its own notes: boot, ok, alert, fail.
    bool playNamed(const char *name) {
        if (cmdarg::is(name, "boot"))  return play("c5:80,e5:80,g5:140");
        if (cmdarg::is(name, "ok"))    return play("g5:60,c6:110");
        if (cmdarg::is(name, "alert")) return play("a5:110,r:60,a5:110");
        if (cmdarg::is(name, "fail"))  return play("g4:140,r:40,c4:240");
        return false;
    }

    // ── Volume ────────────────────────────────────────────────────────────────
    // 0..100 percent, applied as PWM duty (see hal_pwm_tone). 0 is properly
    // silent — the pin is never driven — but timing is unaffected: a muted
    // melody still runs and finishes on schedule, so app logic behaves the same
    // whether or not anyone can hear it.
    void setVolume(uint8_t pct) { _volume = pct > 100 ? 100 : pct; reapply(); }
    uint8_t volume() const { return _volume; }
    bool    muted()  const { return _volume == 0; }

    bool     playing() const { return _mode != Off; }
    uint32_t current() const { return _current; }
    // Times tick() found the pin sounding with nothing scheduled, or had to cut
    // off a runaway tone. Non-zero means something lost a stop — the counter is
    // the evidence trail, since the symptom is otherwise just "it made a noise".
    uint32_t lostStops() const { return _lostStops; }

    // Note name → Hz. Public because it's useful on its own (e.g. mapping a
    // touch coordinate to a pitch). Returns 0 for a rest or an unparseable note.
    static uint32_t noteToHz(const char *s, size_t len) {
        if (!s || len == 0) return 0;
        if ((s[0] | 0x20) == 'r') return 0;                    // rest
        static const uint16_t kBase[12] = {                    // octave 4, Hz
            262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494};
        static const int8_t kSemitone[7] = {9, 11, 0, 2, 4, 5, 7};  // a b c d e f g
        char n = (char)(s[0] | 0x20);
        if (n < 'a' || n > 'g') return 0;
        int semi = kSemitone[n - 'a'];
        size_t i = 1;
        if (i < len && (s[i] == '#' || s[i] == 's')) { semi++; i++; }
        else if (i < len && s[i] == 'b')             { semi--; i++; }
        if (semi < 0)  { semi += 12; }
        if (semi > 11) { semi -= 12; }
        int octave = 4;
        if (i < len && s[i] >= '0' && s[i] <= '8') octave = s[i] - '0';
        uint32_t hz = kBase[semi];
        // Octaves are exact doublings, so shift rather than pow().
        if (octave > 4) hz <<= (octave - 4);
        else if (octave < 4) hz >>= (4 - octave);
        return hz;
    }

private:
    static constexpr int kSeqMax = 96;
    // Nothing this module drives should sound for a minute unattended.
    static constexpr uint64_t kMaxSoundUs = 60ull * 1000000ull;

    enum Mode : uint8_t { Off, Timed, Sequence, Indefinite };

    uint8_t  _pin;
    char     _seq[kSeqMax] = {};
    int      _seqPos  = 0;
    uint64_t _noteEnd = 0;
    Mode     _mode    = Off;
    uint32_t _current = 0;
    uint32_t _toneCalls = 0;
    uint32_t _stopCalls = 0;
    uint32_t _lostStops = 0;
    uint32_t _lastMs    = 0;
    uint8_t  _volume    = 100;      // percent

    // Play the next note in _seq, or stop at the end of the sequence.
    void advance() {
        if (_seqPos >= kSeqMax || !_seq[_seqPos]) { stop(); return; }
        const char *p = _seq + _seqPos;
        while (*p == ',' || *p == ' ') p++;
        if (!*p) { stop(); return; }

        const char *note = p;
        while (*p && *p != ':' && *p != ',') p++;
        size_t noteLen = (size_t)(p - note);

        uint32_t ms = 200;                       // default note length
        if (*p == ':') {
            p++;
            uint32_t v = 0; bool any = false;
            while (*p >= '0' && *p <= '9') { v = v * 10 + (uint32_t)(*p - '0'); p++; any = true; }
            if (any) ms = v;
        }
        while (*p == ',' || *p == ' ') p++;
        _seqPos = (int)(p - _seq);

        uint32_t hz = noteToHz(note, noteLen);
        _current = hz;
        sound(hz);                               // a rest is silence, not a stop
        _noteEnd = hal_time_us() + (uint64_t)ms * 1000;
        _mode    = Sequence;
    }

    static void buzzCmd(const char *args, Writer &out, void *ctx);
    void        dispatch(const char *args, Writer &out);
    void        usage(Writer &out);
};

// Weak app hook — the generated commander_modules.h null-checks and calls this.
extern "C" void commander_on_buzzer_ready(BuzzerModule &) __attribute__((weak));

// ─────────────────────────────────────────────────────────────────────────────

inline void BuzzerModule::usage(Writer &out) {
    out.writeln("buzz                     what's playing");
    out.writeln("buzz <hz> [ms]           a tone (no ms = until 'buzz off')");
    out.writeln("buzz off                 silence");
    out.writeln("buzz beep                a short chirp");
    out.writeln("buzz play <notes>        e.g. c4:200,e4:200,g4:400  ('r' = rest)");
    out.writeln("buzz melody boot|ok|alert|fail");
    out.writeln("buzz vol [0-100]         volume; 0 mutes (bare: report it)");
}

inline void BuzzerModule::dispatch(const char *args, Writer &out) {
    const char *p = cmdarg::skipSpaces(args);

    if (cmdarg::is(p, "help")) { usage(out); return; }

    if (cmdarg::is(p, "off") || cmdarg::is(p, "stop")) {
        stop();
        out.writeln("silent");
        return;
    }
    if (cmdarg::is(p, "beep")) { beep(); out.writeln("beep"); return; }

    if (cmdarg::is(p, "vol") || cmdarg::is(p, "volume")) {
        long v;
        if (!cmdarg::integer(cmdarg::next(p), v, 0, 100)) {
            out.write("volume "); cmdarg::putUInt(out, _volume); out.writeln("%");
            return;
        }
        setVolume((uint8_t)v);
        if (_volume == 0) out.writeln("muted");
        else { out.write("volume "); cmdarg::putUInt(out, _volume); out.writeln("%"); }
        return;
    }

    if (cmdarg::is(p, "play")) {
        const char *q = cmdarg::next(p);
        if (cmdarg::empty(q) || !play(q)) { usage(out); return; }
        out.write("playing "); out.writeln(q);
        return;
    }
    if (cmdarg::is(p, "melody")) {
        const char *q = cmdarg::next(p);
        if (!playNamed(q)) { usage(out); return; }
        out.write("playing "); out.writeln(q);
        return;
    }

    if (cmdarg::empty(p)) {
        static const char *kModes[] = {"off", "timed", "sequence", "indefinite"};
        if (_mode != Off || _current) {
            out.write("playing "); cmdarg::putUInt(out, _current); out.writeln(" Hz");
        } else {
            out.writeln("silent");
        }
        cmdarg::putField(out, "volume",     (int32_t)_volume);
        if (_volume == 0) out.writeln("  (muted — commands still run, silently)");
        cmdarg::putField(out, "mode",       kModes[_mode]);
        cmdarg::putField(out, "tones",      (int32_t)_toneCalls);
        cmdarg::putField(out, "stops",      (int32_t)_stopCalls);
        cmdarg::putField(out, "lost stops", (int32_t)_lostStops);
        cmdarg::putField(out, "last ms",    (int32_t)_lastMs);
        return;
    }

    // Bare number(s): a tone, optionally with a duration.
    long hz;
    const char *q = p;
    if (!cmdarg::integer(q, hz, &q) || hz < 0 || hz > 100000) { usage(out); return; }
    long ms = 0;
    cmdarg::integer(q, ms, 0, 60000);
    tone((uint32_t)hz, (uint32_t)ms);
    out.write("tone "); cmdarg::putUInt(out, (uint32_t)hz); out.write(" Hz");
    if (ms) { out.write(" for "); cmdarg::putUInt(out, (uint32_t)ms); out.write(" ms"); }
    out.writeln();
}

inline void BuzzerModule::buzzCmd(const char *args, Writer &out, void *ctx) {
    static_cast<BuzzerModule *>(ctx)->dispatch(args, out);
}

inline void BuzzerModule::registerCommands(CommandRegistry &reg) {
    reg.registerCommand(CMD(
        "buzz", "buzzer: tone, beep, play a note sequence", I2C_NONE, buzzCmd, this));
}
