#pragma once
#include <stdint.h>
#include <string.h>

// Platform-neutral game-controller vocabulary.
//
// Decoupled from any specific backend (Bluepad32, USB HID, …) so the
// ControllerModule and every consumer speak one stable contract. A backend
// translates its native controller types into a ControllerState; consumers never
// see backend headers. This is the controller equivalent of how i2c_ids.h /
// LocoProtocol.h are the shared vocabulary for the locomotion bridge.

enum GamepadButton : uint8_t {
    BTN_A = 0, BTN_B, BTN_X, BTN_Y,
    BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT,
    BTN_L1, BTN_R1, BTN_L2, BTN_R2,
    BTN_SELECT, BTN_START, BTN_L3, BTN_R3,
    BTN_SYSTEM,                 // the PS / Home / Xbox / guide button
    GAMEPAD_BUTTON_COUNT
};

// One normalized controller sample. Sticks are signed and centered at 0; that
// keeps consumer math simple (a Roomba arcade map is just `vel = -ly`).
struct ControllerState {
    bool     connected = false;
    int16_t  lx = 0, ly = 0;    // left stick,  -512..511 (0 = center)
    int16_t  rx = 0, ry = 0;    // right stick, -512..511 (0 = center)
    uint8_t  lt = 0, rt = 0;    // analog triggers, 0..255
    uint32_t buttons = 0;       // bit (1u << GamepadButton)

    bool pressed(GamepadButton b) const { return (buttons >> b) & 1u; }
};

// Lowercase names used by the `bind <button> …` shell command. Index == enum.
static inline const char *gamepad_button_name(GamepadButton b) {
    static const char *const names[GAMEPAD_BUTTON_COUNT] = {
        "a", "b", "x", "y", "up", "down", "left", "right",
        "l1", "r1", "l2", "r2", "select", "start", "l3", "r3", "system"
    };
    return (b < GAMEPAD_BUTTON_COUNT) ? names[b] : "?";
}

// Parse a button name (expects lowercase). Returns GAMEPAD_BUTTON_COUNT if no
// match. `len` lets callers pass a non-terminated token from a command line.
static inline GamepadButton gamepad_button_from_name(const char *s, size_t len) {
    for (uint8_t i = 0; i < GAMEPAD_BUTTON_COUNT; i++) {
        const char *n = gamepad_button_name((GamepadButton)i);
        if (strlen(n) == len && strncmp(s, n, len) == 0)
            return (GamepadButton)i;
    }
    return GAMEPAD_BUTTON_COUNT;
}
