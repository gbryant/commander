// bp32_pico.h — C shim around Bluepad32 for the Pico 2 W.
//
// Bluepad32's uni_* API is C and its callbacks are plain function pointers, so
// the controller-reading lives here in C. It translates each Bluepad32 sample
// into a backend-neutral bp_raw_t and hands it to a callback the C++ side
// registers. No I2C here — unlike the original pico-bluetooth-bridge (where the
// Pico was an I2C *slave* polled by the R4), in commander the Pico is the I2C
// *master*, so the C++ consumer decides what to do with the input.
#ifndef BP32_PICO_H
#define BP32_PICO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Button bit positions — MUST match GamepadButton in ControllerState.h.
// (Kept as plain macros so this C file and the C++ enum agree on the layout.)
#define BP_BTN_A          0
#define BP_BTN_B          1
#define BP_BTN_X          2
#define BP_BTN_Y          3
#define BP_BTN_DPAD_UP    4
#define BP_BTN_DPAD_DOWN  5
#define BP_BTN_DPAD_LEFT  6
#define BP_BTN_DPAD_RIGHT 7
#define BP_BTN_L1         8
#define BP_BTN_R1         9
#define BP_BTN_L2         10
#define BP_BTN_R2         11
#define BP_BTN_SELECT     12
#define BP_BTN_START      13
#define BP_BTN_L3         14
#define BP_BTN_R3         15
#define BP_BTN_SYSTEM     16

// Backend-neutral controller sample (plain C; mirrors ControllerState fields).
typedef struct {
    bool     connected;
    int16_t  lx, ly, rx, ry;   // -512..511, 0 = center
    uint8_t  lt, rt;           // 0..255
    uint32_t buttons;          // bit (1u << BP_BTN_*)
} bp_raw_t;

typedef void (*bp_update_cb)(const bp_raw_t *sample, void *ctx);

// Register the sink before bp32_init(). Called on every controller sample
// (and on connect/disconnect) from the BTstack async-context worker task.
void bp32_set_callback(bp_update_cb cb, void *ctx);

// Bring up CYW43 + Bluepad32 and start scanning. Non-blocking: under the
// FreeRTOS cyw43_arch, cyw43_arch_init() registers BTstack on the async-context
// worker, so we do NOT call btstack_run_loop_execute(). Returns 0 on success.
int bp32_init(void);

#ifdef __cplusplus
}
#endif

#endif  // BP32_PICO_H
