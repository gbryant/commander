// STM32F207 clock bring-up — conservative first-bring-up default: run from the internal
// 16 MHz HSI RC oscillator, no PLL. This board's HSE crystal (presence/frequency) hasn't
// been confirmed against the schematic yet (see PLAN.md), so this deliberately avoids
// guessing a PLL configuration that could be wrong. HSI is guaranteed present out of
// reset on every F2 part, so this is correct by datasheet, just not fast.
//
// Once the board is in hand: measure/confirm the HSE crystal and switch this to an
// HSE+PLL config for real SYSCLK (up to 120 MHz), the way platform/stm32-bluepill/clock.c
// does for the F103's 72 MHz. That will also need PWR clock + voltage scaling regulator
// setup and ART accelerator / flash wait-state tuning that 16 MHz doesn't require.
#include "stm32f2xx.h"

void stm32_clock_init(void) {
    // SystemInit() has already selected HSI (16 MHz) as SYSCLK out of reset — this just
    // makes that explicit and keeps SystemCoreClock consistent with it.
    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY)) { }

    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_HSI;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSI) { }

    SystemCoreClock = 16000000UL;
}
