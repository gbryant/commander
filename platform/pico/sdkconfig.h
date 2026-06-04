//
// Emulate "menuconfig"
//
#define CONFIG_BLUEPAD32_MAX_DEVICES 4
#define CONFIG_BLUEPAD32_MAX_ALLOWLIST 4
// GAP security. NOTE: Bluepad32 keys off whether this macro is DEFINED, not its
// value, so `#define ... 0` actually selects GAP level 2 (Switch Pro pairs; some
// DS3/DS4 reject with L2CAP 0x66). Undefining it selects level 0 + a per-connection
// level-2 request (DS-friendlier, breaks Switch Pro). Left at the original (defined)
// — a DS4 pairs fine here when it's a *fresh* pairing; the level is not what blocks
// a stale-bonded DS4 (that fails at both levels). Flip only if you need to.
#define CONFIG_BLUEPAD32_GAP_SECURITY 0
#define CONFIG_BLUEPAD32_ENABLE_BLE_BY_DEFAULT 1
// #define CONFIG_BLUEPAD32_ENABLE_VIRTUAL_DEVICE_BY_DEFAULT 1

#define CONFIG_BLUEPAD32_PLATFORM_CUSTOM
#define CONFIG_TARGET_PICO_W

// 2 == Info
#define CONFIG_BLUEPAD32_LOG_LEVEL 2
