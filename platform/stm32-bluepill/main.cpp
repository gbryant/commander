// STM32F103C8 "Bluepill" app config (in-repo testbed).
// Provides commander_config() and commander_setup(); the runner owns main(),
// the clock, console bring-up, FreeRTOS task creation, and panic/DFU plumbing.
#include "commander.h"
#include "core/SystemModule.h"

static SystemModule _m_system;

extern "C" CommanderConfig commander_config() {
    CommanderConfig cfg;
    cfg.uart_baud     = 115200;
    cfg.uart_greeting = "commander/stm32-bluepill";
    return cfg;
}

extern "C" void commander_setup(CommandRegistry &reg) {
    reg.registerModule(_m_system);
}
