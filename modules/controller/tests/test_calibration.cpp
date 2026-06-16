// Host test for modules/controller/ControllerCalibration — the apply() path
// (re-center, rescale to full travel, smooth deadzone) and setIdentity(). The
// interactive run() routine is not exercised here (it's a blocking console walk-
// through); apply() is the hot path every sample goes through. Build via tests/run.sh.
#include <cstdio>
#include <cstdint>
#include "modules/controller/ControllerCalibration.h"

// apply() is pure, but the header pulls in hal.h (run() uses the clock). Stub it.
extern "C" {
    uint64_t hal_time_us(void) { return 0; }
    void     hal_delay_ms(uint32_t) {}
}

static int fails = 0;
static void check(bool ok, const char *what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

static ControllerState stick(int16_t lx, int16_t ly, int16_t rx, int16_t ry) {
    ControllerState s; s.connected = true; s.lx = lx; s.ly = ly; s.rx = rx; s.ry = ry; return s;
}

int main() {
    // ── identity profile: symmetric, centered, light deadzone ───────────────────
    {
        ControllerCalibration c; c.setIdentity();
        check(!c.calibrated(), "setIdentity leaves calibrated() false (not a measured profile)");

        // centered stick -> 0 (inside the deadzone)
        ControllerState o = c.apply(stick(0, 0, 0, 0));
        check(o.lx == 0 && o.ly == 0 && o.rx == 0 && o.ry == 0, "identity: centered stick maps to 0");

        // a value just inside the deadzone (40) clamps to 0
        o = c.apply(stick(30, -30, 20, -10));
        check(o.lx == 0 && o.ly == 0 && o.rx == 0 && o.ry == 0, "identity: sub-deadzone jitter suppressed");

        // full deflection passes through near full-scale, sign preserved
        o = c.apply(stick(511, -512, 511, -512));
        check(o.lx > 480 && o.ly < -480 && o.rx > 480 && o.ry < -480,
              "identity: full deflection reaches near full-scale, sign preserved");

        // monotonic: more input past the deadzone -> more output
        ControllerState a = c.apply(stick(100, 0, 0, 0));
        ControllerState b = c.apply(stick(300, 0, 0, 0));
        check(b.lx > a.lx && a.lx > 0, "identity: output rises monotonically past the deadzone");

        // no jump at the deadzone edge: just past dead is small, not a step to full
        ControllerState edge = c.apply(stick(45, 0, 0, 0));   // dead=40
        check(edge.lx > 0 && edge.lx < 60, "identity: smooth deadzone (no jump at the edge)");
    }

    // ── non-passthrough fields are preserved by apply() ─────────────────────────
    {
        ControllerCalibration c; c.setIdentity();
        ControllerState s = stick(0, 0, 0, 0);
        s.lt = 200; s.rt = 99; s.buttons = (1u << BTN_A) | (1u << BTN_R2); s.connected = true;
        ControllerState o = c.apply(s);
        check(o.lt == 200 && o.rt == 99 && o.buttons == s.buttons && o.connected,
              "apply preserves triggers, buttons, connected flag");
    }

    // ── baked default profile (Wii U Pro): an offset center re-centers to ~0 ────
    {
        ControllerCalibration c;   // default measured profile, NOT identity
        // The default LX center is -45; feeding that raw value should normalize to
        // ~0 (inside the deadzone) rather than reading as a left push.
        ControllerState o = c.apply(stick(-45, 47, -73, 24));   // each axis at its measured center
        check(o.lx == 0 && o.ly == 0 && o.rx == 0 && o.ry == 0,
              "default profile: raw centers normalize to 0 (offset corrected)");
    }

    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails;
}
