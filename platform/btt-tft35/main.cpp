// BTT TFT35-E3 V3.0 (STM32F207) app config (in-repo testbed).
// Provides commander_config() and commander_setup(); the runner owns main(), the clock,
// console bring-up, FreeRTOS task creation, and panic plumbing. Headless-shell scope only
// for now — no LCD/touch driver yet, see PLAN.md.
#include "commander.h"
#include "core/SystemModule.h"

static SystemModule _m_system;

extern "C" CommanderConfig commander_config() {
    CommanderConfig cfg;
    cfg.uart_baud     = 115200;
    cfg.uart_greeting = "commander/btt-tft35";
    return cfg;
}

extern "C" void commander_setup(CommandRegistry &reg) {
    reg.registerModule(_m_system);
}
