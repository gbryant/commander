#pragma once
#include "modules/controller/ControllerBackend.h"
#include "modules/controller/ControllerModule.h"
#include "platform/pico/bp32_pico.h"

// ControllerBackend backed by Bluepad32 on the Pico 2 W. Bridges the C shim
// (bp32_pico.c) to the generic C++ ControllerModule: it registers a C callback
// that converts each bp_raw_t sample into a ControllerState and pushes it to the
// module. begin() is non-blocking (bp32_init starts BTstack on the FreeRTOS
// async-context worker, never runs a blocking run loop).
class PicoBluepadBackend : public ControllerBackend {
public:
    void begin(ControllerModule &sink) override {
        _sink = &sink;
        bp32_set_callback(&trampoline, this);
        bp32_init();
    }

    void forgetKeys() override { bp32_forget_keys(); }

private:
    ControllerModule *_sink = nullptr;

    static void trampoline(const bp_raw_t *r, void *ctx) {
        auto *self = static_cast<PicoBluepadBackend *>(ctx);
        if (!self->_sink) return;
        ControllerState s;
        s.connected = r->connected;
        s.lx = r->lx; s.ly = r->ly; s.rx = r->rx; s.ry = r->ry;
        s.lt = r->lt; s.rt = r->rt;
        s.buttons = r->buttons;          // bit layouts are asserted equal below
        self->_sink->update(s);
    }

    // The C shim builds the button mask with BP_BTN_*; they must equal the
    // GamepadButton enum so the pass-through above is correct.
    static_assert(BP_BTN_A == BTN_A && BP_BTN_B == BTN_B && BP_BTN_L2 == BTN_L2 &&
                  BP_BTN_START == BTN_START && BP_BTN_SYSTEM == BTN_SYSTEM,
                  "bp32_pico.h button bits must match GamepadButton");
};
