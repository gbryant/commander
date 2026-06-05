#pragma once
#include "modules/controller/ControllerState.h"
#include "core/Writer.h"
#include "hal/hal.h"       // hal_time_us, hal_delay_ms (yields)
#include <stdint.h>

// Per-axis stick calibration + smooth deadzone, ported from robot-framework's
// BluetoothController (which gave noticeably smoother driving). It measures each
// stick's resting center, full travel, and natural jitter, so a drifty/asymmetric
// stick normalizes to a clean centered range with no jump at the deadzone edge.
// Bluepad32 already reports -512..511; this corrects the residual offset/asymmetry
// and removes jitter.
//
//   apply(raw)        — normalize a sample (the owning module applies it before
//                       publishing state(), so consumers get clean sticks for free)
//   run(out,sample,…) — the interactive `calibrate` routine: a blocking 4-phase
//                       walk-through (neutral → rotate → neutral → jiggle) that
//                       polls `sample` for the live RAW stick state and narrates
//                       each step. It yields via hal_delay_ms so the controller's
//                       task keeps producing samples. Decoupled from ControllerModule
//                       (takes a sampler) so the module can own a calibration without
//                       a circular include.
//
// v1 keeps calibration in RAM (recalibrate after reboot); it also prints the
// measured values. Flash persistence is a follow-up.
class ControllerCalibration {
public:
    enum Axis { LX, LY, RX, RY, NAXIS };

    bool calibrated() const { return _calibrated; }

    // Re-center, rescale each side to full travel, then apply the deadzone.
    // Safe to call before calibration: the defaults (center 0, full -512..511
    // range, deadzone 40) make this an identity map plus a sane deadzone, so an
    // uncalibrated stick still gets drift suppression.
    ControllerState apply(const ControllerState &s) const {
        ControllerState o = s;
        o.lx = applyAxis(s.lx, LX);
        o.ly = applyAxis(s.ly, LY);
        o.rx = applyAxis(s.rx, RX);
        o.ry = applyAxis(s.ry, RY);
        return o;
    }

    // Sampler the interactive run() polls for the live RAW stick state (must be
    // pre-apply()). The owner passes one that returns its latest raw sample.
    using SampleFn = ControllerState (*)(void *ctx);
    void run(Writer &out, SampleFn sample, void *ctx);

    // Neutralize calibration (center 0, full symmetric range, light deadzone) for a
    // consumer whose controller isn't the baked-in default profile.
    void setIdentity() {
        for (int a = 0; a < NAXIS; a++) { _center[a] = 0; _min[a] = -512; _max[a] = 511; _dead[a] = 40; }
        _calibrated = false;
    }

private:
    // Defaults measured from the primary test controller (Wii U Pro via Bluepad32,
    // 2026-06-04) so apply() gives smooth, drift-free sticks out of the box with no
    // `calibrate` run. These are a reasonable global default; per-pad profiles are a
    // follow-up. Order: LX, LY, RX, RY.
    bool    _calibrated = false;
    int16_t _center[NAXIS] = { -45,   47,  -73,   24};
    int16_t _min[NAXIS]    = {-501, -413, -512, -456};
    int16_t _max[NAXIS]    = { 405,  511,  371,  479};
    int16_t _dead[NAXIS]   = { 150,  102,  148,   98};

    static const char *axisName(int a) {
        switch (a) { case LX: return "LX"; case LY: return "LY"; case RX: return "RX"; default: return "RY"; }
    }
    static int16_t axisOf(const ControllerState &s, int a) {
        switch (a) { case LX: return s.lx; case LY: return s.ly; case RX: return s.rx; default: return s.ry; }
    }

    // Re-center + rescale to full -512..511 (no deadzone). Used both by apply()
    // and by the deadzone-measurement phase.
    int16_t norm(int16_t v, int a) const {
        int16_t c = _center[a];
        int32_t n;
        if (v >= c) n = (_max[a] > c) ? (int32_t)(v - c) * 511 / (_max[a] - c) : 0;
        else        n = (c > _min[a]) ? (int32_t)(v - c) * 512 / (c - _min[a]) : 0;
        if (n > 511)  n = 511;
        if (n < -512) n = -512;
        return (int16_t)n;
    }
    int16_t applyAxis(int16_t v, int a) const {
        int16_t n   = norm(v, a);
        int16_t mag = n < 0 ? (int16_t)-n : n;
        if (mag < _dead[a]) return 0;                       // smooth: dead..511 -> 0..511
        int32_t scaled = (int32_t)(mag - _dead[a]) * 511 / (511 - _dead[a]);
        return n < 0 ? (int16_t)-scaled : (int16_t)scaled;
    }

    // Per-phase countdown printer. Each phase resets _cdSec to -1, then this
    // prints "label Ns" once per whole second of remaining time (most-recent
    // first), so the console shows a live "3 / 2 / 1" without spamming.
    int _cdSec = -1;
    void countdown(Writer &out, const char *label, uint32_t durMs, uint32_t elapsedMs) {
        if (elapsedMs > durMs) elapsedMs = durMs;
        int secLeft = (int)((durMs - elapsedMs + 999) / 1000);
        if (secLeft > 0 && secLeft != _cdSec) {
            _cdSec = secLeft;
            out.write(label); putInt(out, secLeft); out.writeln("s");
        }
    }

    static uint32_t now_ms() { return (uint32_t)(hal_time_us() / 1000); }
    static void putInt(Writer &out, int32_t v) {
        char tmp[12]; int t = 0;
        bool neg = v < 0;
        uint32_t u = neg ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
        if (u == 0) tmp[t++] = '0';
        while (u) { tmp[t++] = (char)('0' + u % 10); u /= 10; }
        char s[13]; int i = 0;
        if (neg) s[i++] = '-';
        while (t) s[i++] = tmp[--t];
        s[i] = '\0';
        out.write(s);
    }
};

inline void ControllerCalibration::run(Writer &out, SampleFn sample, void *ctx) {
    out.writeln("=== controller calibration ===");
    if (!sample(ctx).connected) { out.writeln("no controller connected — pair one first"); return; }

    int32_t  acc[NAXIS]; uint32_t n;
    int16_t  neutral1[NAXIS];

    // ── Phase 1/4: hold neutral (3s) → first center estimate ──────────────────
    out.writeln("1/4: leave BOTH sticks centered, hands off (3s)");
    for (int a = 0; a < NAXIS; a++) acc[a] = 0;
    n = 0;
    _cdSec = -1;
    for (uint32_t start = now_ms(); now_ms() - start < 3000; ) {
        ControllerState s = sample(ctx);
        for (int a = 0; a < NAXIS; a++) acc[a] += axisOf(s, a);
        n++;
        countdown(out, "  hold... ", 3000, now_ms() - start);
        hal_delay_ms(15);
    }
    for (int a = 0; a < NAXIS; a++) neutral1[a] = n ? (int16_t)(acc[a] / (int32_t)n) : 0;

    // ── Phase 2/4: rotate to full extent (8s) → min/max ───────────────────────
    out.writeln("2/4: rotate BOTH sticks in full circles to the edges (8s)");
    { ControllerState s0 = sample(ctx);
      for (int a = 0; a < NAXIS; a++) { _min[a] = axisOf(s0, a); _max[a] = _min[a]; } }
    _cdSec = -1;
    for (uint32_t start = now_ms(); now_ms() - start < 8000; ) {
        ControllerState s = sample(ctx);
        for (int a = 0; a < NAXIS; a++) {
            int16_t v = axisOf(s, a);
            if (v < _min[a]) _min[a] = v;
            if (v > _max[a]) _max[a] = v;
        }
        countdown(out, "  rotate... ", 8000, now_ms() - start);
        hal_delay_ms(15);
    }

    // ── Phase 3/4: hold neutral again (3s) → final center = avg of the two ─────
    out.writeln("3/4: release sticks to center, hands off (3s)");
    for (int a = 0; a < NAXIS; a++) acc[a] = 0;
    n = 0;
    _cdSec = -1;
    for (uint32_t start = now_ms(); now_ms() - start < 3000; ) {
        ControllerState s = sample(ctx);
        for (int a = 0; a < NAXIS; a++) acc[a] += axisOf(s, a);
        n++;
        countdown(out, "  hold... ", 3000, now_ms() - start);
        hal_delay_ms(15);
    }
    for (int a = 0; a < NAXIS; a++) {
        int16_t neutral2 = n ? (int16_t)(acc[a] / (int32_t)n) : 0;
        _center[a] = (int16_t)(((int32_t)neutral1[a] + neutral2) / 2);
    }

    // ── Phase 4/4: jiggle naturally (4s) → deadzone = max normalized jitter ───
    out.writeln("4/4: gently jiggle both sticks as you'd naturally hold them (4s)");
    int16_t jig[NAXIS] = {0, 0, 0, 0};
    _cdSec = -1;
    for (uint32_t start = now_ms(); now_ms() - start < 4000; ) {
        ControllerState s = sample(ctx);
        for (int a = 0; a < NAXIS; a++) {
            int16_t d = norm(axisOf(s, a), a);
            if (d < 0) d = -d;
            if (d > jig[a]) jig[a] = d;
        }
        countdown(out, "  jiggle... ", 4000, now_ms() - start);
        hal_delay_ms(15);
    }
    for (int a = 0; a < NAXIS; a++) {
        int16_t d = (int16_t)(jig[a] + 25);     // margin
        if (d > 150) d = 150;                    // cap
        _dead[a] = d;
    }

    _calibrated = true;
    out.writeln("calibration complete:");
    for (int a = 0; a < NAXIS; a++) {
        out.write("  "); out.write(axisName(a)); out.write(": ");
        putInt(out, _min[a]); out.write(" .. "); putInt(out, _center[a]);
        out.write(" .. "); putInt(out, _max[a]); out.write("   dead="); putInt(out, _dead[a]);
        out.writeln();
    }
}
