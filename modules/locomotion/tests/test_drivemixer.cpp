// Host test for modules/locomotion/ — DriveMixer (two-zone velocity/radius curve,
// velocity ramping, spin-in-place) and the LocoProtocol wire pack/unpack helpers.
// Pure C++, no hardware. A controllable fake clock backs the ramp. Build via tests/run.sh.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include "modules/locomotion/DriveMixer.h"
#include "modules/locomotion/LocoProtocol.h"

// ── fake clock: DriveMixer reads hal_time_us() for its wall-clock ramp ──────────
// Start at a nonzero base: t==0 collides with DriveMixer's _lastMs==0 "first call"
// sentinel, which would re-seed the ramp and swallow a step.
static uint64_t g_now_us = 1000000;
extern "C" {
    uint64_t hal_time_us(void) { return g_now_us; }
    void     hal_delay_ms(uint32_t ms) { g_now_us += (uint64_t)ms * 1000; }
}
static void advance_ms(uint32_t ms) { g_now_us += (uint64_t)ms * 1000; }

static int fails = 0;
static void check(bool ok, const char *what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

// Ramp to steady state: 4 s of 20 ms steps is plenty for any rampRate to converge.
static void settle(DriveMixer &m, int16_t throttle, int16_t steer,
                   int16_t *v, int16_t *r, int spin = 0, int16_t scale = 100) {
    for (int i = 0; i < 200; i++) { advance_ms(20); m.update(throttle, steer, v, r, spin, scale); }
}

int main() {
    // ── velocity is symmetric, monotonic, and two-zone (slow inner / fast outer) ─
    {
        DriveMixer m; int16_t v, r;
        // neutral -> no motion, straight
        m.update(0, 0, &v, &r);
        check(v == 0 && r == LOCO_RADIUS_STRAIGHT, "neutral stick -> stop, straight");

        // Push full forward and let it ramp to steady state (~fastMax, 500 mm/s).
        settle(m, 512, 0, &v, &r);
        check(v >= 500 && r == LOCO_RADIUS_STRAIGHT, "full forward -> ~fastMax velocity, straight");
        int16_t vFullFwd = v;

        // The curve is symmetric to within fixed-point truncation (~1 mm/s): the
        // positive band maps knee..stickFull-1 and the negative -stickFull..-knee,
        // so the two integer-divide segments differ by at most a unit.
        DriveMixer mf, mr; int16_t vf, vr, rf, rr;
        settle(mf, 400, 0, &vf, &rf);
        settle(mr, -400, 0, &vr, &rr);
        int16_t resid = (int16_t)(vf + vr);
        check(vf > 0 && vr < 0 && resid >= -2 && resid <= 2, "curve is symmetric at +/-400 (within truncation)");

        // Inside the knee (fine zone) the steady speed is gentler than at full throttle.
        DriveMixer m3; int16_t vSlow, rs;
        settle(m3, 200, 0, &vSlow, &rs);   // 200/512 ~ 39% < 75% knee
        check(vSlow > 0 && vSlow < vFullFwd, "fine-zone throttle slower than full throttle");
    }

    // ── velocity ramping: target is approached gradually, not instantly ─────────
    {
        DriveMixer m; int16_t v, r;
        m.update(0, 0, &v, &r);          // seed _lastMs
        advance_ms(50);                  // 50ms @ 1500 mm/s^2 -> ~75 mm/s max step
        m.update(512, 0, &v, &r);
        int16_t afterOneStep = v;
        check(afterOneStep > 0 && afterOneStep < 500, "velocity ramps in (not full speed in one 50ms step)");

        for (int i = 0; i < 100; i++) { advance_ms(50); m.update(512, 0, &v, &r); }
        check(v > afterOneStep, "velocity continues ramping toward target over time");

        // Releasing the stick ramps back down to a stop (returns false at standstill).
        bool moving = true;
        for (int i = 0; i < 200 && moving; i++) { advance_ms(50); moving = m.update(0, 0, &v, &r); }
        check(!moving && v == 0, "release ramps down to a full stop");
    }

    // ── two-zone radius: small steer = wide arc, tighter past more steer ─────────
    {
        DriveMixer m; int16_t v, r;
        settle(m, 300, 80, &v, &r);      // gentle steer
        int16_t wide = r;
        settle(m, 300, 300, &v, &r);     // more steer, still under spin knee
        int16_t tight = r;
        // radius sign is negative for one side; compare magnitudes (tighter = smaller |r|)
        int16_t wm = wide < 0 ? -wide : wide;
        int16_t tm = tight < 0 ? -tight : tight;
        check(wm > tm, "more steering yields a tighter arc (smaller radius)");
    }

    // ── steer past the knee snaps to spin-in-place when stickSpin is on ─────────
    {
        DriveMixer m; int16_t v, r;
        settle(m, 300, 500, &v, &r);     // 500/512 ~ 98% > 75% knee, steer right
        check(r == LOCO_RADIUS_CW || r == LOCO_RADIUS_CCW, "steer past knee snaps to a spin code");
    }

    // ── stickSpin=false: steer is arc-only, never a spin ────────────────────────
    {
        DriveMixer::Config cfg; cfg.stickSpin = false;
        DriveMixer m(cfg); int16_t v, r;
        settle(m, 300, 512, &v, &r);     // full steer
        check(r != LOCO_RADIUS_CW && r != LOCO_RADIUS_CCW && r != LOCO_RADIUS_STRAIGHT,
              "stickSpin=false: full steer is an arc, not a spin");
    }

    // ── explicit spin arg: spins in place, steer ignored, speed from |throttle| ─
    {
        DriveMixer m; int16_t v, r;
        settle(m, 400, 0, &v, &r, -1);   // spin clockwise
        check(r == LOCO_RADIUS_CW && v > 0, "spin arg <0 -> CW spin, speed from |throttle|");
        int16_t vSpinFull = v;
        DriveMixer m2; int16_t v2, r2;
        settle(m2, 400, 0, &v2, &r2, +1);// spin counter-clockwise
        check(r2 == LOCO_RADIUS_CCW, "spin arg >0 -> CCW spin");

        // spinScalePct scales the spin speed down (slow/fine mode)
        DriveMixer m3; int16_t vSlow, r3;
        settle(m3, 400, 0, &vSlow, &r3, -1, 50);
        check(vSlow > 0 && vSlow < vSpinFull, "spinScalePct=50 spins slower than full");
    }

    // ── LocoProtocol: drive + sensor snapshots round-trip byte-for-byte ─────────
    {
        uint8_t buf[LOCO_DRIVE_LEN];
        int16_t vel, rad;
        loco_pack_drive(250, LOCO_RADIUS_STRAIGHT, buf);
        loco_unpack_drive(buf, &vel, &rad);
        check(vel == 250 && rad == LOCO_RADIUS_STRAIGHT, "drive pack/unpack round-trips (straight code)");

        loco_pack_drive(-500, -1, buf);
        loco_unpack_drive(buf, &vel, &rad);
        check(vel == -500 && rad == -1, "drive pack/unpack round-trips negatives");
        // big-endian on the wire
        check(buf[0] == 0xFE && buf[1] == 0x0C, "drive velocity is big-endian on the wire");

        LocoSensors s; s.flags1 = LOCO_F1_BUMP_LEFT | LOCO_F1_WALL; s.flags2 = LOCO_F2_CLIFF_FR;
        s.distance_mm = -1234; s.angle_deg = 567; s.battery_pct = 88; s.charging_state = 3;
        s.voltage_mv = 16200; s.current_ma = -450;
        uint8_t sb[LOCO_SENSORS_LEN]; LocoSensors o;
        loco_pack_sensors(&s, sb); loco_unpack_sensors(sb, &o);
        bool ok = o.flags1 == s.flags1 && o.flags2 == s.flags2 && o.distance_mm == s.distance_mm &&
                  o.angle_deg == s.angle_deg && o.battery_pct == s.battery_pct &&
                  o.charging_state == s.charging_state && o.voltage_mv == s.voltage_mv &&
                  o.current_ma == s.current_ma;
        check(ok, "sensor snapshot pack/unpack round-trips all fields");
    }

    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails;
}
