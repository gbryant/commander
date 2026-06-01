// STM32F103C8 "Bluepill" platform main (in-repo testbed, analogous to platform/arduino/main.cpp).
// Phase 2: command shell over USART1 (PA9/PA10). No USB yet — Phase 3 swaps the console to
// USB CDC behind COMMANDER_STM32_USB_CONSOLE. I2C-backed modules (compass) wait for Phase 4.
#include "stm32f1xx.h"
#include "FreeRTOS.h"
#include "task.h"
#include "core/CommandRegistry.h"
#include "core/SystemModule.h"
#include "transport/uart/UartTransport.h"
#include "hal/hal.h"

extern "C" void stm32_clock_init(void);
#ifdef COMMANDER_STM32_USB_CONSOLE
extern "C" void usb_hw_init(void);     // platform/stm32-bluepill/usb.c
extern "C" void usbd_task(void *);
#endif
#ifdef COMMANDER_STM32_DFU
extern "C" uint32_t _estack;           // top of RAM (linker symbol)
#endif

static CommandRegistry registry;
static SystemModule    systemModule;
static UartTransport   uart;

// PC13 = onboard LED (active low).
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

// In USB-console builds the panic hooks must NOT touch the console: hal_uart_puts() would
// take TinyUSB's OSAL mutex from a fault context and could deadlock (cf. the Pico lesson).
// They blink instead.
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

#ifdef COMMANDER_STM32_USB_CONSOLE
    usb_hw_init();                          // enable USB peripheral + IRQ, nudge enumeration
#endif

    uart.begin(registry, 115200, "commander/stm32-bluepill");   // USART1, or no-op on USB

    registry.registerModule(systemModule);
#ifdef COMMANDER_STM32_DFU
    // `bootloader`: drop into the davidgfnet DFU bootloader so the app can be re-flashed
    // over USB (dfu-util). Magic word at top-of-RAM-8 is what the bootloader checks at boot.
    registry.registerCommand(CMD("bootloader", "reboot into the USB DFU bootloader", CMD_BOOTLOADER,
        [](const char *, Writer &out, void *) {
            out.writeln("entering DFU bootloader...");
            hal_delay_ms(80);               // let the line flush over USB before we reset
            *(volatile uint64_t *)((uint8_t *)&_estack - 8) = 0xDEADBEEFCC00FFEEULL;
            __DSB();
            NVIC_SystemReset();
        }, nullptr));
#endif
    registry.validateIds();

#ifdef COMMANDER_STM32_USB_CONSOLE
    xTaskCreate(usbd_task, "usbd", 512, nullptr, 3, nullptr);    // pump tud_task()
#endif
    xTaskCreate(UartTransport::taskBody, "uart", 256, &uart, 2, nullptr);
    vTaskStartScheduler();

    for (;;) { }   // only reached if the scheduler failed to start
}
