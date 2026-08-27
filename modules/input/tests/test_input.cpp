// Host tests for the input/output modules — JoystickModule (normalization,
// deadzone, direction, bindings), ButtonsModule (time debounce, edges, bindings),
// LedModule (state, blink), BuzzerModule (note parsing, non-blocking playback).
// Run via tests/run.sh.
//
// The debounce and note-sequence logic in particular are things that are painful
// to characterize on hardware and trivial to pin down here, because the fake HAL
// lets a test move the clock.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include "tests/fakes/fake_hal.h"
#include "modules/input/JoystickModule.h"
#include "modules/input/ButtonsModule.h"
#include "modules/LedModule.h"
#include "modules/BuzzerModule.h"
#include "core/CommandRegistry.h"

static int fails = 0;
static void check(bool ok, const char *what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

class StringWriter : public Writer {
public:
    void write(const char *s) override   { text += s; }
    void writeln(const char *s) override { text += s; text += "\n"; }
    std::string text;
};

// A command the tests can bind to, so bindings are observed through the real
// registry rather than a stub.
static int g_marks = 0;
static void markCmd(const char *, Writer &, void *) { g_marks++; }
static void registerMark(CommandRegistry &reg) {
    reg.registerCommand(CMD("mark", "test marker", I2C_NONE, markCmd, nullptr));
}

// Debounce is time-based: one tick observes the level change, a later tick
// (past the window) promotes it to an event. This helper does both, the way a
// real 50 Hz ticker would.
static void settle(ButtonsModule &b, uint32_t ms = 40) {
    b.tick();
    fake_hal::now_us += (uint64_t)ms * 1000;
    b.tick();
}

static uint32_t lastTone() {
    uint32_t hz = 0;
    for (const auto &e : fake_hal::log) {
        if (e.kind == fake_hal::Event::PwmTone) hz = e.value;
        else if (e.kind == fake_hal::Event::PwmStop) hz = 0;
    }
    return hz;
}

// Duty cycle of the most recent tone — this is what volume actually controls.
static uint32_t lastDuty() {
    uint32_t duty = 0;
    for (const auto &e : fake_hal::log)
        if (e.kind == fake_hal::Event::PwmTone) duty = e.aux;
    return duty;
}

int main() {
    // ═══ Joystick ════════════════════════════════════════════════════════════
    // NOTE: registerModule() calls init(), which calibrates. So register first
    // (with the stick centred), then move it — exactly the order a real board
    // powers up in.
    {
        fake_hal::reset();
        JoystickModule j(26, 27);           // GP26/27 → channels 0/1
        CommandRegistry reg; StringWriter w;
        reg.registerModule(j);              // init() → centre measured at 2048
        check(j.ready(), "joy: init finds ADC channels for GP26/GP27");
        check(j.x() == 0 && j.y() == 0, "joy: resting stick normalizes to 0,0");
        check(j.direction() == JoyDir::Center, "joy: resting stick reads Center");

        fake_hal::setAdc(0, 4095);
        reg.dispatch("joy", w);
        check(j.x() > 990, "joy: full right deflection reaches +1000");
        check(j.direction() == JoyDir::Right, "joy: full right reads Right");

        fake_hal::setAdc(0, 0);
        reg.dispatch("joy", w);
        check(j.x() < -990, "joy: full left deflection reaches -1000");
        check(j.direction() == JoyDir::Left, "joy: full left reads Left");

        fake_hal::setAdc(0, 2048); fake_hal::setAdc(1, 4095);
        reg.dispatch("joy", w);
        check(j.direction() == JoyDir::Up, "joy: full up reads Up");
        fake_hal::setAdc(1, 0);
        reg.dispatch("joy", w);
        check(j.direction() == JoyDir::Down, "joy: full down reads Down");
    }

    // An off-centre resting position must still normalize to zero AND still
    // reach full scale at both rails — that's the two-sided scaling.
    {
        fake_hal::reset();
        fake_hal::setAdc(0, 1000); fake_hal::setAdc(1, 3000);
        JoystickModule off(26, 27);
        CommandRegistry reg; StringWriter w;
        reg.registerModule(off);            // centre measured at 1000 / 3000
        check(off.x() == 0 && off.y() == 0, "joy: off-centre rest position calibrates to 0");

        fake_hal::setAdc(0, 4095);
        reg.dispatch("joy", w);
        check(off.x() > 990, "joy: off-centre stick still reaches +1000 at the rail");
        fake_hal::setAdc(0, 0);
        reg.dispatch("joy", w);
        check(off.x() < -990, "joy: off-centre stick still reaches -1000 at the other rail");
    }

    // deadzone + diagonal disambiguation
    {
        fake_hal::reset();
        JoystickModule j(26, 27);
        CommandRegistry reg; StringWriter w;
        reg.registerModule(j);

        // 10% off centre, default deadzone 30% → still Center.
        fake_hal::setAdc(0, 2048 + 205);
        reg.dispatch("joy", w);
        check(j.direction() == JoyDir::Center, "joy: small movement inside the deadzone is Center");

        // Diagonal: x further out than y → Right, not both.
        fake_hal::setAdc(0, 4095); fake_hal::setAdc(1, 3000);
        reg.dispatch("joy", w);
        check(j.direction() == JoyDir::Right, "joy: diagonal resolves to the dominant axis");
        fake_hal::setAdc(0, 3000); fake_hal::setAdc(1, 4095);
        reg.dispatch("joy", w);
        check(j.direction() == JoyDir::Up, "joy: diagonal the other way resolves to the other axis");

        // A narrower deadzone makes the same small movement register.
        fake_hal::setAdc(0, 2048 + 205); fake_hal::setAdc(1, 2048);
        j.setDeadzone(5);
        reg.dispatch("joy", w);
        check(j.direction() == JoyDir::Right, "joy: deadzone 5% lets a small movement through");
    }

    // joystick tick → callback + binding
    {
        fake_hal::reset();
        JoystickModule j(26, 27);
        CommandRegistry reg;
        registerMark(reg);
        reg.registerModule(j);

        static int calls = 0; static JoyDir last = JoyDir::Center;
        calls = 0;
        j.onDirection([](JoyDir d, void *) { calls++; last = d; });

        StringWriter w;
        reg.dispatch("joy bind right mark", w);
        check(w.text.find("bound right") != std::string::npos, "joy: bind reports the binding");

        g_marks = 0;
        fake_hal::setAdc(0, 4095);
        fake_hal::now_us += 100000;
        j.tick();
        check(calls == 1 && last == JoyDir::Right, "joy: tick fires onDirection once per change");
        check(g_marks == 1, "joy: binding dispatched the bound command");

        fake_hal::now_us += 100000;
        j.tick();
        check(calls == 1 && g_marks == 1, "joy: holding the same direction doesn't repeat");

        fake_hal::setAdc(0, 2048);
        fake_hal::now_us += 100000;
        j.tick();
        check(calls == 2 && last == JoyDir::Center, "joy: returning to centre is a change");

        w.text.clear();
        reg.dispatch("joy unbind right", w);
        g_marks = 0;
        fake_hal::setAdc(0, 4095);
        fake_hal::now_us += 100000;
        j.tick();
        check(g_marks == 0, "joy: unbind stops the dispatch");
    }

    // a platform with no ADC says so instead of reporting a centred stick
    {
        fake_hal::reset();
        JoystickModule j(10, 11);          // not ADC pins
        j.init();
        check(!j.ready(), "joy: non-ADC pins leave the module not-ready");
        CommandRegistry reg; StringWriter w;
        reg.registerModule(j);
        reg.dispatch("joy", w);
        check(w.text.find("no ADC") != std::string::npos, "joy: reports missing ADC rather than faking a reading");
    }

    // ═══ Buttons ═════════════════════════════════════════════════════════════
    {
        fake_hal::reset();
        const uint8_t pins[] = {15, 14};
        ButtonsModule b(pins, 2, true, 25);   // active-low, 25 ms debounce
        fake_hal::setGpio(15, true);          // pulled up = released
        fake_hal::setGpio(14, true);
        b.init();
        check(b.count() == 2, "btn: two buttons configured");
        check(!b.pressed(0) && !b.pressed(1), "btn: both start released");

        static int events = 0; static int lastIdx = -1; static bool lastDown = false;
        events = 0;
        b.onPress([](uint8_t i, bool down, void *) { events++; lastIdx = i; lastDown = down; });

        // Press button 0 (goes low). Nothing until the debounce window elapses.
        fake_hal::setGpio(15, false);
        b.tick();
        check(events == 0 && !b.pressed(0), "btn: a fresh edge is not reported immediately");

        fake_hal::now_us += 10000;            // 10 ms — still inside the window
        b.tick();
        check(events == 0, "btn: still nothing 10 ms in");

        fake_hal::now_us += 20000;            // 30 ms total — settled
        b.tick();
        check(events == 1 && lastIdx == 0 && lastDown, "btn: reports the press once settled");
        check(b.pressed(0), "btn: pressed() reflects the debounced state");
        check(b.presses(0) == 1, "btn: press counter incremented");

        // Bounce: rapid chatter inside the window must not produce events.
        fake_hal::setGpio(15, true);
        b.tick();
        fake_hal::now_us += 5000;
        fake_hal::setGpio(15, false);
        b.tick();
        fake_hal::now_us += 5000;
        fake_hal::setGpio(15, true);
        b.tick();
        fake_hal::now_us += 5000;
        b.tick();
        check(events == 1, "btn: contact bounce produces no extra events");

        // Held released long enough → one release event.
        fake_hal::now_us += 40000;
        b.tick();
        check(events == 2 && !lastDown, "btn: reports the release once settled");
        check(!b.pressed(0), "btn: back to released");

        // The other button is independent.
        fake_hal::setGpio(14, false);
        settle(b);
        check(events == 3 && lastIdx == 1 && lastDown, "btn: buttons are independent");
    }

    // button bindings + shell
    {
        fake_hal::reset();
        const uint8_t pins[] = {15};
        ButtonsModule b(pins, 1);
        CommandRegistry reg;
        registerMark(reg);
        reg.registerModule(b);
        fake_hal::setGpio(15, true);
        b.init();

        StringWriter w;
        reg.dispatch("btn bind 0 mark", w);
        check(w.text.find("bound button 0") != std::string::npos, "btn: bind reports the binding");

        g_marks = 0;
        fake_hal::setGpio(15, false);
        settle(b);
        check(g_marks == 1, "btn: press dispatched the bound command");

        fake_hal::setGpio(15, true);
        settle(b);
        check(g_marks == 1, "btn: release does not dispatch the binding");

        w.text.clear();
        reg.dispatch("btn", w);
        check(w.text.find("presses=1") != std::string::npos, "btn: status shows the press count");

        w.text.clear();
        reg.dispatch("btn bind 9 mark", w);
        check(w.text.find("btn bind") != std::string::npos, "btn: out-of-range index prints usage");
    }

    // ═══ LEDs ════════════════════════════════════════════════════════════════
    {
        fake_hal::reset();
        const uint8_t pins[] = {16, 17};
        LedModule l(pins, 2);
        l.init();
        check(l.count() == 2 && !l.state(0), "led: starts off");

        CommandRegistry reg; StringWriter w;
        reg.registerModule(l);

        reg.dispatch("led 0 on", w);
        check(l.state(0), "led: 'led 0 on' turns it on");
        reg.dispatch("led 0 toggle", w);
        check(!l.state(0), "led: toggle flips it back");
        reg.dispatch("led all on", w);
        check(l.state(0) && l.state(1), "led: 'led all on'");

        // Blink toggles at the requested period, driven from tick().
        reg.dispatch("led 1 blink 100", w);
        bool s0 = l.state(1);
        fake_hal::now_us += 50000;
        l.tick();
        check(l.state(1) == s0, "led: blink doesn't toggle before the period elapses");
        fake_hal::now_us += 60000;
        l.tick();
        check(l.state(1) != s0, "led: blink toggles after the period");

        reg.dispatch("led 1 off", w);
        fake_hal::now_us += 200000;
        l.tick();
        check(!l.state(1), "led: an explicit off cancels blinking");

        w.text.clear();
        reg.dispatch("led 5 on", w);
        check(w.text.find("led <n>") != std::string::npos, "led: out-of-range index prints usage");
    }

    // ═══ Buzzer ══════════════════════════════════════════════════════════════
    {
        // Note table: A4 is 440, and octaves are exact doublings.
        check(BuzzerModule::noteToHz("a4", 2) == 440, "buzz: a4 = 440 Hz");
        check(BuzzerModule::noteToHz("a5", 2) == 880, "buzz: a5 = 880 Hz");
        check(BuzzerModule::noteToHz("a3", 2) == 220, "buzz: a3 = 220 Hz");
        check(BuzzerModule::noteToHz("c4", 2) == 262, "buzz: c4 = 262 Hz");
        check(BuzzerModule::noteToHz("c#4", 3) == 277, "buzz: sharps raise a semitone");
        check(BuzzerModule::noteToHz("db4", 3) == 277, "buzz: flats and sharps agree enharmonically");
        check(BuzzerModule::noteToHz("r", 1) == 0, "buzz: 'r' is a rest");
        check(BuzzerModule::noteToHz("x9", 2) == 0, "buzz: nonsense note is silent, not garbage");
        check(BuzzerModule::noteToHz("a", 1) == 440, "buzz: a bare note defaults to octave 4");

        fake_hal::reset();
        BuzzerModule b(13);
        b.init();
        CommandRegistry reg; StringWriter w;
        reg.registerModule(b);

        reg.dispatch("buzz 1000", w);
        check(lastTone() == 1000, "buzz: 'buzz 1000' sounds 1000 Hz");
        check(b.playing(), "buzz: an open-ended tone counts as playing (tick must watch it)");
        reg.dispatch("buzz off", w);
        check(lastTone() == 0, "buzz: 'buzz off' silences");

        // A timed tone stops itself from tick(), without blocking.
        fake_hal::reset();
        reg.dispatch("buzz 500 100", w);
        check(lastTone() == 500 && b.playing(), "buzz: timed tone starts and is playing");
        fake_hal::now_us += 50000;
        b.tick();
        check(lastTone() == 500, "buzz: still sounding before the duration elapses");
        fake_hal::now_us += 60000;
        b.tick();
        check(lastTone() == 0 && !b.playing(), "buzz: timed tone stops itself");

        // A melody advances note by note from tick().
        fake_hal::reset();
        check(b.play("c4:100,e4:100,g4:200"), "buzz: play accepts a sequence");
        check(lastTone() == 262, "buzz: first note sounds immediately");
        fake_hal::now_us += 110000;
        b.tick();
        check(lastTone() == 330, "buzz: advances to the second note");
        fake_hal::now_us += 110000;
        b.tick();
        check(lastTone() == 392, "buzz: advances to the third note");
        fake_hal::now_us += 210000;
        b.tick();
        check(lastTone() == 0 && !b.playing(), "buzz: stops at the end of the sequence");

        // A rest is silence in the middle of a sequence, not the end of it.
        fake_hal::reset();
        b.play("a4:50,r:50,a4:50");
        check(lastTone() == 440, "buzz: rest sequence starts on the note");
        fake_hal::now_us += 60000;
        b.tick();
        check(lastTone() == 0 && b.playing(), "buzz: rest is silent but still playing");
        fake_hal::now_us += 60000;
        b.tick();
        check(lastTone() == 440 && b.playing(), "buzz: sounds again after the rest");

        // Named melodies and error handling.
        fake_hal::reset();
        w.text.clear();
        reg.dispatch("buzz melody boot", w);
        check(lastTone() != 0, "buzz: 'buzz melody boot' plays");
        w.text.clear();
        reg.dispatch("buzz melody nope", w);
        check(w.text.find("buzz melody") != std::string::npos, "buzz: unknown melody prints usage");
        w.text.clear();
        reg.dispatch("buzz play", w);
        check(w.text.find("buzz play") != std::string::npos, "buzz: 'play' with no notes prints usage");

        // The watchdog: a tone left sounding with nothing scheduled must be
        // recovered by tick(), not left on forever. This is the failure that
        // reached hardware — a stuck buzzer needing a reboot.
        fake_hal::reset();
        b.stop();
        reg.dispatch("buzz 1500", w);            // open-ended tone
        check(lastTone() == 1500 && b.playing(), "buzz: open-ended tone sounds");
        fake_hal::now_us += 61ull * 1000000ull;  // past the safety cap
        b.tick();
        check(lastTone() == 0 && !b.playing(), "buzz: runaway tone is cut off by the safety cap");
        check(b.lostStops() >= 1, "buzz: the cut-off is counted as a lost stop");

        // The diagnostic counter has to see sequence notes too, or a melody
        // looks like it played nothing (which is how it read on hardware).
        fake_hal::reset();
        b.stop();
        uint32_t before = b.tonesStarted();
        b.play("c4:50,r:50,e4:50");           // two notes and a rest
        fake_hal::now_us += 60000; b.tick();
        fake_hal::now_us += 60000; b.tick();
        fake_hal::now_us += 60000; b.tick();
        check(b.tonesStarted() == before + 2, "buzz: a melody counts its notes, rests excluded");

        // ── Volume ───────────────────────────────────────────────────────────
        // Loudness on a piezo is duty cycle, so volume must reach the pin — and
        // 0 must be genuinely silent, not just quiet.
        fake_hal::reset();
        b.stop();
        check(b.volume() == 100 && !b.muted(), "buzz: full volume by default");
        b.tone(1000, 100);
        uint32_t loud = lastDuty();
        check(loud > 0, "buzz: a tone carries a duty cycle");

        fake_hal::reset();
        b.setVolume(50);
        b.tone(1000, 100);
        check(lastDuty() > 0 && lastDuty() < loud, "buzz: half volume lowers the duty cycle");

        fake_hal::reset();
        b.setVolume(0);
        check(b.muted(), "buzz: volume 0 reads as muted");
        b.tone(1000, 100);
        check(lastTone() == 0, "buzz: muted means the pin is never driven");

        // Muting must not distort timing — a silent melody still runs and ends
        // on schedule, so app logic behaves the same either way.
        fake_hal::reset();
        b.play("c4:100,e4:100");
        check(b.playing(), "buzz: a muted melody still plays (silently)");
        check(lastTone() == 0, "buzz: ...and makes no sound");
        fake_hal::now_us += 110000; b.tick();
        check(b.playing(), "buzz: muted melody advances to the second note");
        fake_hal::now_us += 110000; b.tick();
        check(!b.playing(), "buzz: muted melody finishes on schedule");

        // Turning the volume up mid-note takes effect immediately.
        fake_hal::reset();
        b.setVolume(100);
        b.tone(880, 500);
        check(lastTone() == 880, "buzz: unmuting restores sound");
        fake_hal::reset();
        b.setVolume(0);
        check(lastTone() == 0, "buzz: muting mid-note silences it at once");
        b.stop();
        b.setVolume(100);

        // A very long sequence is truncated rather than overflowing the buffer.
        std::string longSeq;
        for (int i = 0; i < 60; i++) longSeq += "c4:10,";
        fake_hal::reset();
        b.play(longSeq.c_str());
        for (int i = 0; i < 200 && b.playing(); i++) { fake_hal::now_us += 20000; b.tick(); }
        check(!b.playing(), "buzz: an over-long sequence terminates instead of running away");
    }

    printf("\n%s\n", fails ? "FAILURES" : "all input tests passed");
    return fails ? 1 : 0;
}
