// BTT TFT35-E3 V3.0 (STM32F207) runner — the library entry point for downstream `cmdr init
// btt-tft35` projects (mirrors runners/stm32-bluepill/runner.cpp). The app provides
// commander_config()/commander_setup(); this owns main(), the clock, the USART1 console,
// FreeRTOS task creation, and panic plumbing.
//
// Build flag: -DCOMMANDER_TFT35_RUNNER (select this runner).
//
// UNVERIFIED ON HARDWARE — see hal/stm32f2/hal.cpp and PLAN.md for what's still open
// (HSE crystal, whether PA9/PA10 are broken out, the status LED pin). No LCD/touch/SD
// support here yet; this only brings up a shell console.
#ifdef COMMANDER_TFT35_RUNNER

#include "stm32f2xx.h"
#include "FreeRTOS.h"
#include "task.h"
#include "commander.h"
#include "core/CommandRegistry.h"
#include "core/Writer.h"
#include "i2c_ids.h"
#include "hal/hal.h"
#include "transport/uart/UartTransport.h"
#include "platform/btt-tft35/stm32_panic.h"

extern "C" void stm32_clock_init(void);

static CommanderConfig _cfg;
static CommandRegistry _registry;
static UartTransport   _uart;

extern "C" __attribute__((weak)) void commander_early_init()                   {}
extern "C" __attribute__((weak)) void commander_on_uart_ready(UartTransport &) {}

int main(void) {
    stm32_clock_init();
    commander_early_init();
    _cfg = commander_config();

    const char *greeting = _cfg.uart_greeting ? _cfg.uart_greeting : "commander";
    _uart.begin(_registry, _cfg.uart_baud, greeting);   // USART1

    if (_cfg.i2c_sda >= 0)
        hal_i2c_init((uint8_t)_cfg.i2c_sda, (uint8_t)_cfg.i2c_scl, _cfg.i2c_hz);

    commander_setup(_registry);
    _registry.registerCommand(CMD("reset", "reboot the firmware", CMD_RESET,
        [](const char *, Writer &out, void *) {
            out.writeln("Rebooting...");
            hal_delay_ms(50);
            NVIC_SystemReset();
        }, nullptr));
    _registry.validateIds();
    commander_on_uart_ready(_uart);
    commander_run_autostart(_registry);   // boot commands (cmdr autostart)

    xTaskCreate(UartTransport::taskBody, "uart", 256, &_uart, 2, nullptr);
    vTaskStartScheduler();

    for (;;) { }
}

#endif  // COMMANDER_TFT35_RUNNER
