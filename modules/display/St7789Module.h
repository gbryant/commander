#pragma once
#include "modules/display/SpiPanel.h"

// ── ST7789 / ST7789V TFT panel ───────────────────────────────────────────────
// The controller behind most small colour IPS modules: the 1.14" 240x135 on the
// Waveshare RP2350-GEEK and RP2040-GEEK, the 1.3" 240x240, the 1.9" 320x170, and
// the 135x240 breakouts. Everything except the bring-up sequence and the MADCTL
// table lives in SpiPanel.
//
// **The thing that bites on these panels is the window offset.** The controller
// has 240x320 of RAM and the glass shows a smaller window of it, so every
// address needs shifting — and the shift changes with rotation. SpiPanel derives
// it from the gap between `ramW/ramH` and `nativeW/nativeH` (see the comment
// there), which reproduces the familiar numbers for a 240x135 panel: 52,40 at
// rotation 0 and 40,53 at rotation 1. Panels that don't centre their window are
// corrected live with `lcd offset <dx> <dy>` — run `lcd test` and nudge until
// the 1px border touches all four edges.
//
// Sizing a config for a particular module means setting nativeW/nativeH to the
// glass in its portrait orientation and ramW/ramH to the controller's RAM:
//
//   240x135 (1.14", GEEK)   nativeW 135, nativeH 240, ramW 240, ramH 320
//   240x240 (1.3")          nativeW 240, nativeH 240, ramW 240, ramH 320
//   320x170 (1.9")          nativeW 170, nativeH 320, ramW 240, ramH 320
//   240x320 (2.0", full)    nativeW 240, nativeH 320  (no gap, offsets zero)

class St7789Module : public SpiPanel {
public:
    explicit St7789Module(const SpiPanelConfig &cfg) : SpiPanel(cfg) {}

    // Colour order. ST7789 modules are usually wired RGB, but BGR ones exist and
    // present as red/blue swapped — set this rather than editing a table.
    void setBgr(bool bgr) { _bgr = bgr; }

protected:
    const char *panelName() const override { return "st7789"; }

    // 90° per step, the standard ST7789 progression. Bit 3 selects BGR order.
    uint8_t madctl(uint8_t rot) const override {
        static const uint8_t kMadctl[4] = {0x00, 0x60, 0xC0, 0xA0};
        return (uint8_t)(kMadctl[rot & 3] | (_bgr ? 0x08 : 0x00));
    }

    void sendInitSequence() override {
        // ST7789 datasheet bring-up. Unlike the ST7796 there's no command-set
        // unlock; the power and gamma registers are directly writable.
        static const InitCmd kInit[] = {
            {0x01, {0}, 0},                       // SWRESET (followed by a delay)
            {0x3A, {0x55}, 1},                    // COLMOD: 16 bit/pixel (RGB565)
            {0xB2, {0x0C, 0x0C, 0x00, 0x33, 0x33}, 5},  // PORCTRL: porch control
            {0xB7, {0x35}, 1},                    // GCTRL: VGH 13.26V / VGL -10.43V
            {0xBB, {0x19}, 1},                    // VCOMS: 0.725V
            {0xC0, {0x2C}, 1},                    // LCMCTRL
            {0xC2, {0x01}, 1},                    // VDVVRHEN: VDV/VRH from registers
            {0xC3, {0x12}, 1},                    // VRHS: 4.45V
            {0xC4, {0x20}, 1},                    // VDVS: 0V
            {0xC6, {0x0F}, 1},                    // FRCTRL2: 60 Hz frame rate
            {0xD0, {0xA4, 0xA1}, 2},              // PWCTRL1
            {0xE0, {0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54,
                    0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23}, 14},          // PVGAMCTRL
            {0xE1, {0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44,
                    0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23}, 14},          // NVGAMCTRL
        };
        // SWRESET needs settling time before the rest; runInit's trailing delay
        // covers the end of the sequence, so give the reset its own here.
        cmd(0x01);
        hal_delay_ms(150);
        runInit(kInit + 1, sizeof(kInit) / sizeof(kInit[0]) - 1);
    }

private:
    bool _bgr = false;
};
