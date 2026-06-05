#pragma once
#include "modules/controller/ControllerState.h"
#include "hal/hal.h"       // hal_time_us
#include <stdint.h>

// Temporal low-pass for analog sticks — a first-order EMA per axis that tames the
// per-sample jitter raw gamepad readings carry (e.g. a worn stick reads ±40 units
// even when pinned, which a linear velocity map turns into visible speed wobble).
// Complements ControllerCalibration: filter = temporal smoothing, calibration =
// spatial re-center/rescale/deadzone. They compose (filter first, then calibrate).
//
// Rate-independent: the smoothing is set by a wall-clock time constant (hal_time_us
// dt), not a per-sample weight, so it behaves the same whatever rate the controller
// reports at (~100–250 Hz, controller-dependent). tauMs is the knob — larger =
// smoother but laggier; ~80 ms is what the robot validated as smooth. Buttons and
// triggers pass through untouched.
class StickFilter {
public:
    explicit StickFilter(uint16_t tauMs = 80) : _tau(tauMs ? tauMs : 1) {}

    ControllerState apply(const ControllerState &s) {
        if (!s.connected) { reset(); return s; }   // neutral immediately on dropout
        uint32_t now = (uint32_t)(hal_time_us() / 1000);
        if (!_primed) {                             // snap to the first live sample
            _primed = true; _last = now;
            _st[0] = (int32_t)s.lx << 8; _st[1] = (int32_t)s.ly << 8;
            _st[2] = (int32_t)s.rx << 8; _st[3] = (int32_t)s.ry << 8;
            return s;
        }
        uint32_t dt = now - _last;
        _last = now;
        if (dt == 0) dt = 1;
        ControllerState o = s;
        o.lx = step(s.lx, _st[0], dt);
        o.ly = step(s.ly, _st[1], dt);
        o.rx = step(s.rx, _st[2], dt);
        o.ry = step(s.ry, _st[3], dt);
        return o;
    }

    void     reset() { _primed = false; _last = 0; _st[0] = _st[1] = _st[2] = _st[3] = 0; }
    void     setTau(uint16_t tauMs) { _tau = tauMs ? tauMs : 1; }
    uint16_t tau() const { return _tau; }

private:
    uint16_t _tau;
    bool     _primed = false;
    uint32_t _last = 0;
    int32_t  _st[4] = {0, 0, 0, 0};   // per-axis EMA state, axis << 8 (8 frac bits)

    // EMA: state += alpha*(x - state), alpha = dt/(tau+dt). int64 intermediate so a
    // large dt (after a pause) can't overflow; large dt → alpha→1 → snaps, as wanted.
    int16_t step(int16_t x, int32_t &state, uint32_t dt) {
        int32_t target = (int32_t)x << 8;
        state += (int32_t)(((int64_t)(target - state) * dt) / (_tau + dt));
        return (int16_t)(state >> 8);
    }
};
