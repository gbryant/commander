//
// Emulate "menuconfig"
//
#define CONFIG_BLUEPAD32_MAX_DEVICES 4
#define CONFIG_BLUEPAD32_MAX_ALLOWLIST 4
// GAP security. Bluepad32 keys off whether this macro is DEFINED, not its value, so
// `#define ... 0` actually selects GAP level 2. Level 2 lets the Nintendo Switch Pro
// pair but REJECTS *fresh* pairing of the Wii U Pro (RVL-CNT-01-UC), DS3 and DS4 with
// L2CAP 0x66 ("Set GAP security to 2"). Leaving it UNDEFINED selects level 0 + a
// per-connection level-2 request, which fresh-pairs the Wii U Pro / DS-style pads
// (HW-confirmed) at the cost of the Switch Pro. UNDEFINED here — the Wii U Pro is the
// primary controller; Switch Pro support is a later, runtime-selectable follow-up.
// NOTE: this only gates FRESH SSP pairing — an already-bonded pad reconnects either
// way, which is why a level-2 build seemed fine until `btforget` cleared the bond.
// #define CONFIG_BLUEPAD32_GAP_SECURITY 0
#define CONFIG_BLUEPAD32_ENABLE_BLE_BY_DEFAULT 1
// #define CONFIG_BLUEPAD32_ENABLE_VIRTUAL_DEVICE_BY_DEFAULT 1

#define CONFIG_BLUEPAD32_PLATFORM_CUSTOM
#define CONFIG_TARGET_PICO_W

// 2 == Info
#define CONFIG_BLUEPAD32_LOG_LEVEL 2
