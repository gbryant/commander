// STM32F103C8 "Bluepill" runner — the library entry point for downstream `cmdr init
// bluepill` projects (mirrors runners/arduino-uno/runner.cpp, but native CMSIS). The app
// provides commander_config()/commander_setup(); this owns main(), the clock, the console
// (USART1 or USB CDC), FreeRTOS task creation, and the panic/DFU plumbing.
//
// Build flags: -DCOMMANDER_BLUEPILL_RUNNER (select this runner),
//   -DCOMMANDER_STM32_USB_CONSOLE (USB CDC console instead of USART1),
//   -DCOMMANDER_STM32_DFU (app runs above the DFU bootloader @ 0x08001000).
#ifdef COMMANDER_BLUEPILL_RUNNER

#include "stm32f1xx.h"
#include "FreeRTOS.h"
#include "task.h"
#include "commander.h"
#include "core/CommandRegistry.h"
#include "core/Writer.h"
#include "i2c_ids.h"
#include "hal/hal.h"
#include "transport/uart/UartTransport.h"

extern "C" void stm32_clock_init(void);
#ifdef COMMANDER_STM32_USB_CONSOLE
extern "C" void usb_hw_init(void);
extern "C" void usbd_task(void *);
#endif
#ifdef COMMANDER_STM32_DFU
extern "C" uint32_t _estack;           // top of RAM (linker symbol)
#endif

static CommanderConfig _cfg;
static CommandRegistry _registry;
static UartTransport   _uart;

extern "C" __attribute__((weak)) void commander_early_init()                   {}
extern "C" __attribute__((weak)) void commander_on_uart_ready(UartTransport &) {}

// PC13 = onboard LED (active low) — used by the panic hook.
static void led_init(void) {
    RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;
    GPIOC->CRH = (GPIOC->CRH & ~(0xFu << 20)) | (0x2u << 20);
}
static inline void led(bool on) { GPIOC->BSRR = on ? GPIO_BSRR_BR13 : GPIO_BSRR_BS13; }

// Override the weak hook in core/CommandRegistry.cpp: blink fast forever on panic.
void commander_on_panic() {
    for (;;) {
        led(true);  for (volatile uint32_t i = 0; i < 400000; i++) { }
        led(false); for (volatile uint32_t i = 0; i < 400000; i++) { }
    }
}

// On USB-console builds the hooks must NOT touch the console (TinyUSB OSAL mutex from a
// fault context could deadlock) — they blink instead.
extern "C" void vApplicationMallocFailedHook(void) {
#ifndef COMMANDER_STM32_USB_CONSOLE
    hal_uart_puts("[PANIC] malloc failed\r\n");
#endif
    commander_on_panic();
}
extern "C" void vApplicationStackOverflowHook(TaskHandle_t, char *name) {
#ifndef COMMANDER_STM32_USB_CONSOLE
    hal_uart_puts("[PANIC] stack overflow: ");
    hal_uart_puts(name ? name : "?");
    hal_uart_puts("\r\n");
#else
    (void)name;
#endif
    commander_on_panic();
}

int main(void) {
    stm32_clock_init();
#ifdef COMMANDER_STM32_DFU
    SCB->VTOR = 0x08001000;                 // app runs above the 4 KB DFU bootloader
#endif
    led_init();
    commander_early_init();
    _cfg = commander_config();

#ifdef COMMANDER_STM32_USB_CONSOLE
    usb_hw_init();                          // enable USB peripheral + IRQ, nudge enumeration
#endif

    const char *greeting = _cfg.uart_greeting ? _cfg.uart_greeting : "commander";
    _uart.begin(_registry, _cfg.uart_baud, greeting);   // USART1, or no-op on USB

    if (_cfg.i2c_sda >= 0)
        hal_i2c_init((uint8_t)_cfg.i2c_sda, (uint8_t)_cfg.i2c_scl, _cfg.i2c_hz);

    commander_setup(_registry);
#ifdef COMMANDER_STM32_DFU
    // `bootloader`: reboot into the DFU bootloader so the app can be re-flashed over USB.
    _registry.registerCommand(CMD("bootloader", "reboot into the USB DFU bootloader", CMD_BOOTLOADER,
        [](const char *, Writer &out, void *) {
            out.writeln("entering DFU bootloader...");
            hal_delay_ms(80);
            *(volatile uint64_t *)((uint8_t *)&_estack - 8) = 0xDEADBEEFCC00FFEEULL;
            __DSB();
            NVIC_SystemReset();
        }, nullptr));
#endif
    _registry.validateIds();
    commander_on_uart_ready(_uart);

#ifdef COMMANDER_STM32_USB_CONSOLE
    xTaskCreate(usbd_task, "usbd", 512, nullptr, 3, nullptr);
#endif
    xTaskCreate(UartTransport::taskBody, "uart", 256, &_uart, 2, nullptr);
    vTaskStartScheduler();

    for (;;) { }
}

#endif  // COMMANDER_BLUEPILL_RUNNER
