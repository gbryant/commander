// STM32F103 clock bring-up: HSE 8 MHz crystal -> PLL ×9 -> 72 MHz SYSCLK, with the
// USB clock landing on 48 MHz (PLL / 1.5). Called at the top of main() before the
// FreeRTOS scheduler starts. We configure explicitly rather than trust the framework's
// SystemInit, since the FreeRTOS tick (configCPU_CLOCK_HZ) assumes a true 72 MHz core.
#include "stm32f1xx.h"

void stm32_clock_init(void) {
    // Flash: enable prefetch buffer and 2 wait states (required for 48 < SYSCLK <= 72 MHz).
    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_2;

    // Start the external 8 MHz crystal and wait for it to stabilise.
    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY)) { }

    // PLL source = HSE (not /2), ×9 => 72 MHz.  USBPRE=0 => USB clock = PLL/1.5 = 48 MHz.
    RCC->CFGR &= ~(RCC_CFGR_PLLSRC | RCC_CFGR_PLLXTPRE | RCC_CFGR_PLLMULL | RCC_CFGR_USBPRE);
    RCC->CFGR |= RCC_CFGR_PLLSRC | RCC_CFGR_PLLMULL9;

    // Bus prescalers: AHB /1 (72), APB1 /2 (36 — its max), APB2 /1 (72), ADC /6 (12).
    RCC->CFGR &= ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2 | RCC_CFGR_ADCPRE);
    RCC->CFGR |= RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_PPRE2_DIV1 | RCC_CFGR_ADCPRE_DIV6;

    // Enable the PLL, wait for lock, then switch SYSCLK to it.
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) { }

    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) { }

    SystemCoreClock = 72000000UL;
}
