#pragma once
#include "modules/locomotion/LocoProtocol.h"
#include "hal/hal.h"        // hal_time_us
#include <stdint.h>

// Smooth differential-drive mixing, ported from robot-framework's Bluetooth
// controller — the source of its noticeably smooth feel. Converts two normalized
// stick axes (throttle + steering, each in -stickFull..stickFull, e.g. straight
// out of ControllerCalibration::apply) into a (velocity mm/s, radius) Roomba/loco
// drive command, with the three ingredients that make it smooth:
//
//   • two-zone velocity — the inner fastKneePct% of throttle maps to a gentle
//     ±slowMax for fine control; the outer band boosts to ±fastMax.
//   • two-zone radius   — small steering = a wide arc (arcWide→arcTight mm) that
//     tightens as you push; past the knee it snaps to a spin in place
//     (LOCO_RADIUS_CW/CCW).
//   • velocity ramping  — the commanded velocity slews toward target at rampRate
//     (mm/s²), so the base eases in on a flick and coasts to a stop on release
//     instead of jerking.
//
// Pure and input-source-agnostic — it knows nothing about controllers or I2C, so
// any input (gamepad, web, autonomy) can feed it. Hold one instance per drive base
// (it carries the ramp state) and call update() at a steady-ish rate; the ramp is
// wall-clock based (hal_time_us), so an uneven call rate still ramps correctly.
class DriveMixer {
public:
    struct Config {
        int16_t stickFull   = 512;   // |axis| full-scale of the throttle/steer inputs
        int16_t fastKneePct = 75;    // % of travel before velocity boosts / steer spins
        int16_t slowMax     = 250;   // mm/s at the knee (fine-control zone ceiling)
        int16_t fastMax     = 500;   // mm/s at full throttle
        int16_t arcWide     = 400;   // turn radius (mm) at the start of the steer band
        int16_t arcTight    = 100;   // turn radius (mm) at the knee (tightest arc)
        int16_t rampRate    = 1500;  // velocity slew limit, mm/s per second
        bool    stickSpin   = true;  // steer past the knee snaps to a spin. false =
                                     // steer is arc-ONLY (full deflection = tightest
                                     // arc); use the spin arg for spin-in-place instead.
    };

    DriveMixer() {}                                       // all-default tuning
    explicit DriveMixer(const Config &cfg) : _cfg(cfg) {}

    // throttle/steer in -stickFull..stickFull. `spin`: 0 = normal drive; <0 = spin
    // clockwise, >0 = spin counter-clockwise in place — then steer is ignored and
    // |throttle| sets the spin wheel speed (so a stick can be "spin speed" while a
    // trigger holds the spin). Writes the ramped command to *velOut/*radiusOut.
    // Returns true while commanding motion, false once it has ramped to a full stop
    // — the caller sends a stop on the true→false edge.
    bool update(int16_t throttle, int16_t steer, int16_t *velOut, int16_t *radiusOut, int spin = 0) {
        int16_t targetVel    = 0;
        int16_t targetRadius = LOCO_RADIUS_STRAIGHT;
        if (spin != 0) {                                  // trigger spin: |throttle| = speed
            targetRadius = (spin < 0) ? LOCO_RADIUS_CW : LOCO_RADIUS_CCW;
            targetVel    = spinSpeed(throttle < 0 ? (int16_t)-throttle : throttle);
        } else if (throttle != 0 || steer != 0) {
            targetVel    = toVelocity(throttle);
            targetRadius = toRadius(steer);
        }

        uint32_t now = (uint32_t)(hal_time_us() / 1000);
        if (_lastMs == 0) _lastMs = now;                 // first call: no jump
        int32_t maxStep = (int32_t)_cfg.rampRate * (int32_t)(now - _lastMs) / 1000;
        if (maxStep < 1) maxStep = 1;
        _lastMs = now;

        int16_t diff = (int16_t)(targetVel - _vel);
        if (diff > maxStep)       _vel = (int16_t)(_vel + maxStep);
        else if (diff < -maxStep) _vel = (int16_t)(_vel - maxStep);
        else                      _vel = targetVel;

        *velOut    = _vel;
        *radiusOut = targetRadius;
        return _vel != 0;
    }

    // Drop the ramp state (e.g. on a controller dropout) so the next update()
    // ramps from a standstill instead of a stale velocity.
    void reset() { _vel = 0; _lastMs = 0; }

private:
    Config   _cfg;
    int16_t  _vel    = 0;     // current ramped velocity we're commanding
    uint32_t _lastMs = 0;     // last update() time for the ramp delta

    static int16_t map16(int16_t x, int16_t inLo, int16_t inHi, int16_t outLo, int16_t outHi) {
        return (int16_t)((int32_t)(x - inLo) * (outHi - outLo) / (inHi - inLo) + outLo);
    }

    int16_t toVelocity(int16_t v) const {
        int16_t knee = (int16_t)((int32_t)_cfg.stickFull * _cfg.fastKneePct / 100);
        int16_t a    = v < 0 ? (int16_t)-v : v;
        if (a <= knee) return map16(v, (int16_t)-knee, knee, (int16_t)-_cfg.slowMax, _cfg.slowMax);
        if (v > 0)     return map16(v, knee, (int16_t)(_cfg.stickFull - 1), _cfg.slowMax, _cfg.fastMax);
        return map16(v, (int16_t)-_cfg.stickFull, (int16_t)-knee, (int16_t)-_cfg.fastMax, (int16_t)-_cfg.slowMax);
    }

    int16_t toRadius(int16_t v) const {
        if (v == 0) return LOCO_RADIUS_STRAIGHT;
        int16_t knee = (int16_t)((int32_t)_cfg.stickFull * _cfg.fastKneePct / 100);
        int16_t a    = v < 0 ? (int16_t)-v : v;
        // Past the knee = spin in place — but ONLY when stickSpin is enabled. With
        // it off the stick is arc-only (spin lives on the `spin` arg / a trigger).
        if (_cfg.stickSpin && a > knee)
            return v < 0 ? LOCO_RADIUS_CCW : LOCO_RADIUS_CW;
        // Wide arc, tightening toward the knee (stickSpin) or full deflection (off).
        int16_t hi = _cfg.stickSpin ? knee : (int16_t)(_cfg.stickFull - 1);
        if (a > hi) a = hi;                               // |v| can hit stickFull (e.g. -512)
        int16_t r = map16(a, 1, hi, _cfg.arcWide, _cfg.arcTight);
        return (int16_t)(v < 0 ? r : -r);                 // left = +radius (turn CCW)
    }

    // |throttle| → spin wheel speed (mm/s), 0..fastMax. Linear so the stick is a
    // proportional "spin speed" while a trigger holds the spin.
    int16_t spinSpeed(int16_t mag) const {
        if (mag <= 0) return 0;
        if (mag > _cfg.stickFull) mag = _cfg.stickFull;
        return (int16_t)((int32_t)mag * _cfg.fastMax / _cfg.stickFull);
    }
};
