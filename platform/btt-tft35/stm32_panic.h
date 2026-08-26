#pragma once
// Shared panic definitions for the BTT TFT35-E3 V3.0 (STM32F207).
// Include from exactly ONE translation unit per build (platform main or runner).
// Callers must include stm32f2xx.h, FreeRTOS.h, task.h, and hal/hal.h first.
//
// Unlike platform/stm32-bluepill/stm32_panic.h, this doesn't blink an onboard LED —
// this board's status LED GPIO hasn't been confirmed against the schematic yet (see
// PLAN.md). Panic just halts after printing, which is still diagnosable over the
// console. Add an led()/led_init() pair here once the pin is confirmed.

// Override the weak hook in core/CommandRegistry.cpp.
void commander_on_panic() {
    for (;;) { }
}

extern "C" void vApplicationMallocFailedHook(void) {
    hal_uart_puts("[PANIC] malloc failed\r\n");
    commander_on_panic();
}
extern "C" void vApplicationStackOverflowHook(TaskHandle_t, char *name) {
    hal_uart_puts("[PANIC] stack overflow: ");
    hal_uart_puts(name ? name : "?");
    hal_uart_puts("\r\n");
    commander_on_panic();
}
