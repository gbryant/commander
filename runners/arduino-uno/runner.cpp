#ifdef COMMANDER_UNO_RUNNER

#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include "commander.h"
#include "hal/hal.h"

static CommanderConfig _cfg;
static CommandRegistry _registry;
static UartTransport   _uart;
static StackType_t     _uartStack[192];
static StaticTask_t    _uartTCB;

extern "C" __attribute__((weak)) void commander_early_init()                   {}
extern "C" __attribute__((weak)) void commander_on_uart_ready(UartTransport &) {}
extern "C" __attribute__((weak)) void commander_on_wifi_connected()            {}

void setup() {
    commander_early_init();
    _cfg = commander_config();

    const char *greeting = _cfg.uart_greeting ? _cfg.uart_greeting : "commander";
    _uart.begin(_registry, _cfg.uart_baud, greeting);

    if (_cfg.i2c_sda >= 0)
        hal_i2c_init((uint8_t)_cfg.i2c_sda, (uint8_t)_cfg.i2c_scl, _cfg.i2c_hz);

    commander_setup(_registry);
    _registry.validateIds();

    commander_on_uart_ready(_uart);
    xTaskCreateStatic(UartTransport::taskBody, "uart", 192, &_uart, 2, _uartStack, &_uartTCB);
}

void loop() {}

#endif // COMMANDER_UNO_RUNNER
