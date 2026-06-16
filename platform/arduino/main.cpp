// Arduino Uno in-repo testbed — app config for the Uno runner.
// Provides commander_config(), commander_setup(), and commander_on_uart_ready();
// the runner owns setup(), task creation, and panic hooks.
#include "commander.h"
#include "core/SystemModule.h"
#include "modules/CompassModule.h"
#include "modules/I2CDiagModule.h"
#include "modules/SonarModule.h"
#include "IRModule.h"

static SystemModule  _m_system;
static CompassModule _m_compass;
static I2CDiagModule _m_i2c;
static SonarModule   _m_sonar(6);  // Grove D6
static IRModule      _m_ir;

extern "C" CommanderConfig commander_config() {
    CommanderConfig cfg;
    cfg.uart_baud     = 115200;
    cfg.uart_greeting = "commander/uno";
    cfg.i2c_sda       = SDA;
    cfg.i2c_scl       = SCL;
    cfg.i2c_hz        = 400000;
    return cfg;
}

extern "C" void commander_setup(CommandRegistry &reg) {
    reg.registerModule(_m_system);
    reg.registerModule(_m_compass);
    reg.registerModule(_m_i2c);
    reg.registerModule(_m_sonar);
    reg.registerModule(_m_ir);
}

extern "C" void commander_on_uart_ready(UartTransport &uart) {
    uart.addTicker(_m_ir);
}
