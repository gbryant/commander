// Minimal in-tree ESP32 demo for commander: the built-in `help`/`version`
// commands over native USB-serial, nothing else. The runner (runners/esp32)
// owns app_main, FreeRTOS wiring, WiFi/telnet, and the weak hooks.
//
// Real projects are scaffolded with `cmdr init <name> esp32` and grow sensors
// via `cmdr module enable` (e.g. `ina219`, `i2c`, `wifi`) — that path generates
// commander_modules.h and a commander_setup() that registers the chosen modules.
// This file is the hand-written equivalent for the smallest possible demo.
//
// The former solar-monitor app that lived here (hand-wired INA219 + WiFi/mDNS)
// has moved to its own consumer repo, cmdr-solar-monitor; INA219 is now a
// first-class cmdr module (`cmdr module enable ina219`).
#include "commander.h"
#include "core/SystemModule.h"

static SystemModule sysModule;

extern "C" CommanderConfig commander_config() {
    CommanderConfig cfg;
    cfg.uart_baud     = 115200;
    cfg.uart_greeting = "commander/esp32s3";
    // wifi_ssid stays null → the runner skips all networking for this demo.
    return cfg;
}

extern "C" void commander_setup(CommandRegistry& reg) {
    reg.registerModule(sysModule);
}
