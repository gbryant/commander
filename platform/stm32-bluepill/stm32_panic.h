#pragma once
// Shared panic + LED definitions for STM32F103 Bluepill.
// Include from exactly ONE translation unit per build (platform main or runner).
// Callers must include stm32f1xx.h, FreeRTOS.h, task.h, and hal/hal.h first.

// PC13 = onboard LED (active low).
static void led_init(void) {
    RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;
    GPIOC->CRH = (GPIOC->CRH & ~(0xFu << 20)) | (0x2u << 20);
}
static inline void led(bool on) { GPIOC->BSRR = on ? GPIO_BSRR_BR13 : GPIO_BSRR_BS13; }

// Override the weak hook in core/CommandRegistry.cpp: blink fast forever on panic.
void commander_on_panic() {
    for (;;) {
        led(true);  for (volatile uint32_t i = 0; i < 400000; i++) {}
        led(false); for (volatile uint32_t i = 0; i < 400000; i++) {}
    }
}

// In USB-console builds the panic hooks must NOT touch the console: hal_uart_puts()
// would take TinyUSB's OSAL mutex from a fault context and could deadlock.
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
