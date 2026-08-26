#pragma once
#include "modules/display/SpiPanel.h"

// ── ST7796S / ST7796SU1 TFT panel ────────────────────────────────────────────
// The 3.5" 320x480 panel on the GeeekPi Pico Breadboard Kit, and the same
// controller on plenty of other boards. Everything except the bring-up sequence
// and the MADCTL table lives in SpiPanel.
//
// The controller's RAM matches the glass here (320x480), so the window offsets
// SpiPanel derives are all zero — unlike the ST7789 modules, which show a small
// window of a larger RAM.

// Kept as a distinct name so existing configs and the cmdr emitter read clearly.
using St7796Config = SpiPanelConfig;

class St7796Module : public SpiPanel {
public:
    explicit St7796Module(const SpiPanelConfig &cfg) : SpiPanel(cfg) {}

protected:
    const char *panelName() const override { return "st7796"; }

    // 90° per step. 0x48 is the panel's natural portrait orientation (MX + BGR).
    uint8_t madctl(uint8_t rot) const override {
        static const uint8_t kMadctl[4] = {0x48, 0x28, 0x88, 0xE8};
        return kMadctl[rot & 3];
    }

    void sendInitSequence() override {
#ifdef ST7796_LEGACY_INIT
        // The vendor's table, kept verbatim as an escape hatch. It's an ILI9341
        // sequence that the GeeekPi demo shipped for this ST7796 panel — the
        // controller ignores the commands it doesn't know, and the shared ones
        // (MADCTL/COLMOD/SLPOUT/DISPON) are enough to bring it up. If the
        // datasheet sequence below ever misbehaves on a panel variant, build with
        // -DST7796_LEGACY_INIT to fall back to what was known to work.
        static const InitCmd kInit[] = {
            {0xCF, {0x00, 0x83, 0x30}, 3},
            {0xED, {0x64, 0x03, 0x12, 0x81}, 4},
            {0xE8, {0x85, 0x01, 0x79}, 3},
            {0xCB, {0x39, 0x2C, 0x00, 0x34, 0x02}, 5},
            {0xF7, {0x20}, 1},
            {0xEA, {0x00, 0x00}, 2},
            {0xC0, {0x26}, 1},
            {0xC1, {0x11}, 1},
            {0xC5, {0x35, 0x3E}, 2},
            {0xC7, {0xBE}, 1},
            {0x3A, {0x05}, 1},
            {0xB1, {0x00, 0x1B}, 2},
            {0xF2, {0x08}, 1},
            {0x26, {0x01}, 1},
            {0xE0, {0x1F, 0x1A, 0x18, 0x0A, 0x0F, 0x06, 0x45, 0x87, 0x32, 0x0A, 0x07, 0x02, 0x07, 0x05, 0x00}, 15},
            {0xE1, {0x00, 0x25, 0x27, 0x05, 0x10, 0x09, 0x3A, 0x78, 0x4D, 0x05, 0x18, 0x0D, 0x38, 0x3A, 0x1F}, 15},
            {0xB7, {0x07}, 1},
            {0xB6, {0x0A, 0x82, 0x27, 0x00}, 4},
        };
#else
        // ST7796S datasheet bring-up. 0xF0 is CSCON — the command-set-control
        // gate that unlocks (0xC3/0x96) and re-locks (0x3C/0x69) the manufacturer
        // commands; without it the power/VCOM/gamma writes below are ignored.
        static const InitCmd kInit[] = {
            {0xF0, {0xC3}, 1},                    // CSCON: unlock part 1
            {0xF0, {0x96}, 1},                    // CSCON: unlock part 2
            {0x3A, {0x55}, 1},                    // COLMOD: 16 bit/pixel (RGB565)
            {0xB4, {0x01}, 1},                    // DIC: 1-dot inversion
            {0xB6, {0x80, 0x02, 0x3B}, 3},        // DFC: display function control
            {0xB7, {0xC6}, 1},                    // EM: entry mode
            {0xC0, {0x80, 0x45}, 2},              // PWR1
            {0xC1, {0x13}, 1},                    // PWR2: VGH/VGL
            {0xC2, {0xA7}, 1},                    // PWR3
            {0xC5, {0x0A}, 1},                    // VCMPCTL: VCOM
            {0xE8, {0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33}, 8},   // DOCA
            {0xE0, {0xD0, 0x08, 0x0F, 0x06, 0x06, 0x33, 0x30, 0x33,
                    0x47, 0x17, 0x13, 0x13, 0x2B, 0x31}, 14},              // PGC
            {0xE1, {0xD0, 0x0A, 0x11, 0x0B, 0x09, 0x07, 0x2F, 0x33,
                    0x47, 0x38, 0x15, 0x16, 0x2C, 0x32}, 14},              // NGC
            {0xF0, {0x3C}, 1},                    // CSCON: lock part 1
            {0xF0, {0x69}, 1},                    // CSCON: lock part 2
        };
#endif
        runInit(kInit, sizeof(kInit) / sizeof(kInit[0]));
    }
};
