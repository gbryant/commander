// Arduino R4 WiFi in-repo testbed — app config for the R4 runner.
// Provides commander_config() and commander_setup(); the runner owns setup(),
// WiFi, mDNS, telnet, and task creation.
#include "commander.h"
#include "core/SystemModule.h"
#include "secrets.h"

static SystemModule _m_system;

extern "C" CommanderConfig commander_config() {
    CommanderConfig cfg;
    cfg.wifi_ssid     = WIFI_SSID;
    cfg.wifi_password = WIFI_PASSWORD;
    cfg.hostname      = "commander-r4";
    cfg.uart_baud     = 115200;
    cfg.uart_greeting = "commander/r4";
    return cfg;
}

extern "C" void commander_setup(CommandRegistry &reg) {
    reg.registerModule(_m_system);
}
