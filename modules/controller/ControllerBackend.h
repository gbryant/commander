#pragma once

class ControllerModule;

// The platform seam for the controller module. A backend produces controller
// samples (from Bluetooth via Bluepad32, USB HID, a test harness, …) and pushes
// them into the module by calling sink.update(state) on each new sample.
//
// Mirrors the IR split (modules/ir/IIRModule.h abstract ↔ platform/pico concrete):
// the generic ControllerModule depends only on this interface, so swapping the
// input source touches no consumer code.
//
// ⚠️ begin() MUST be non-blocking. CommandRegistry::registerModule() calls
// init() (→ begin()) before registerCommands(), and on the Pico the BTstack run
// loop never returns — so begin() must start a task/async context and return,
// never run the loop inline.
class ControllerBackend {
public:
    virtual ~ControllerBackend() = default;
    virtual void begin(ControllerModule &sink) = 0;

    // Clear stored pairings so a controller with a stale/mismatched bond can
    // re-pair. Default no-op — backends without persistent bonds (USB, test
    // harness) need do nothing; the Bluetooth backend overrides it.
    virtual void forgetKeys() {}
};
