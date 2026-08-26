// STM32F2 native HAL — implements hal/hal.h against CMSIS registers (no vendor HAL).
// Target: BIGTREETECH TFT35 V3.0/E3/B1 (STM32F207VC — Cortex-M3, 256 KB flash, 128 KB
// SRAM). Modeled on hal/stm32/hal.cpp (F103 "Bluepill"), but F2's GPIO block is the
// MODER/OTYPER/OSPEEDR/PUPDR/AFR style shared with F4, not F1's CRL/CRH nibbles.
//
// UNVERIFIED ON HARDWARE. This board's schematic hasn't been checked yet — see
// PLAN.md. Specifically unconfirmed before first flash attempt:
//   - Whether PA9/PA10 (USART1, AF7 — the default pin pair used below) are actually
//     broken out on a header on this board, or reserved for the LCD/touch bus.
//   - The onboard status LED GPIO (none is wired into commander_on_panic() here —
//     see platform/btt-tft35/stm32_panic.h).
// Both are safe to get wrong: worst case is "no console output", not damage.
//
// Pin encoding: a pin is (port << 4) | bit, where port 0..4 = GPIOA..GPIOE.
// Time base: the Cortex-M3 DWT cycle counter (free-running at SystemCoreClock),
// same approach as hal/stm32 — F1 and F2 share the Cortex-M3 core.
// I2C: stubbed, same as hal/stm32 (compass is the only consumer so far).
#include "../hal.h"
#include "stm32f2xx.h"
#include "FreeRTOS.h"
#include "task.h"

// --- helpers -----------------------------------------------------------------

static GPIO_TypeDef *port_of(uint8_t pin) {
    switch (pin >> 4) {
        case 0:  return GPIOA;
        case 1:  return GPIOB;
        case 2:  return GPIOC;
        case 3:  return GPIOD;
        default: return GPIOE;
    }
}

static void enable_port_clock(uint8_t pin) {
    switch (pin >> 4) {
        case 0:  RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN; break;
        case 1:  RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN; break;
        case 2:  RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN; break;
        case 3:  RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN; break;
        default: RCC->AHB1ENR |= RCC_AHB1ENR_GPIOEEN; break;
    }
}

// Write the 2-bit MODER field for one pin: 0=input, 1=output, 2=AF, 3=analog.
static void set_pin_mode(uint8_t pin, uint32_t mode2) {
    GPIO_TypeDef *g = port_of(pin);
    uint8_t bit = pin & 0xF;
    g->MODER = (g->MODER & ~(0x3u << (bit * 2))) | (mode2 << (bit * 2));
}

// Select an alternate function (0-15) for one pin via AFR[0] (pins 0-7) / AFR[1] (8-15).
static void set_pin_af(uint8_t pin, uint32_t af) {
    GPIO_TypeDef *g = port_of(pin);
    uint8_t bit = pin & 0xF;
    uint8_t idx = bit < 8 ? 0 : 1;
    uint8_t sh  = (bit % 8) * 4;
    g->AFR[idx] = (g->AFR[idx] & ~(0xFu << sh)) | (af << sh);
}

static bool s_dwt_ready = false;
static void dwt_init(void) {
    if (s_dwt_ready) return;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    s_dwt_ready = true;
}

// --- GPIO --------------------------------------------------------------------

void hal_gpio_set_output(uint8_t pin) {
    enable_port_clock(pin);
    set_pin_mode(pin, 0x1);   // general-purpose output, push-pull (OTYPER default = 0)
}

void hal_gpio_set_input(uint8_t pin) {
    enable_port_clock(pin);
    set_pin_mode(pin, 0x0);   // input, PUPDR default = no pull
}

void hal_gpio_write(uint8_t pin, bool high) {
    uint8_t bit = pin & 0xF;
    port_of(pin)->BSRR = high ? (1u << bit) : (1u << (bit + 16));
}

bool hal_gpio_read(uint8_t pin) {
    return (port_of(pin)->IDR >> (pin & 0xF)) & 1u;
}

uint32_t hal_pulse_in_us(uint8_t pin, bool level, uint32_t timeout_us) {
    dwt_init();
    GPIO_TypeDef *g = port_of(pin);
    uint8_t bit = pin & 0xF;
    uint32_t cyc_per_us  = SystemCoreClock / 1000000UL;
    uint32_t timeout_cyc = timeout_us * cyc_per_us;
    uint32_t t0 = DWT->CYCCNT;

    // Wait for any in-progress pulse of this level to end.
    while ((((g->IDR >> bit) & 1u) != 0) == level)
        if (DWT->CYCCNT - t0 > timeout_cyc) return 0;
    // Wait for the pulse to begin.
    while ((((g->IDR >> bit) & 1u) != 0) != level)
        if (DWT->CYCCNT - t0 > timeout_cyc) return 0;
    uint32_t start = DWT->CYCCNT;
    // Wait for the pulse to end.
    while ((((g->IDR >> bit) & 1u) != 0) == level)
        if (DWT->CYCCNT - t0 > timeout_cyc) return 0;

    return (DWT->CYCCNT - start) / cyc_per_us;
}

// --- Time --------------------------------------------------------------------

void hal_delay_ms(uint32_t ms) {
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        vTaskDelay(pdMS_TO_TICKS(ms));
    } else {
        dwt_init();
        uint64_t target = hal_time_us() + (uint64_t)ms * 1000ULL;
        while (hal_time_us() < target) { }
    }
}

// 64-bit µs clock built from the 32-bit DWT counter, extended by accumulating deltas.
// Not reentrant — only the UART task uses it (mirrors hal/stm32).
uint64_t hal_time_us(void) {
    static uint64_t accum = 0;
    static uint32_t last  = 0;
    dwt_init();
    uint32_t now = DWT->CYCCNT;
    accum += (uint32_t)(now - last);
    last   = now;
    return accum / (SystemCoreClock / 1000000UL);
}

// --- UART console --------------------------------------------------------------
// USART1: PA9 = TX, PA10 = RX, AF7 — the standard F2/F4 default USART1 pinout, unverified
// against this specific board (see file header).

void hal_uart_init(uint32_t baud) {
    dwt_init();
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

    set_pin_mode(0x09, 0x2); set_pin_af(0x09, 7);   // PA9  = USART1_TX, AF7
    set_pin_mode(0x0A, 0x2); set_pin_af(0x0A, 7);   // PA10 = USART1_RX, AF7

    // USART1 is on APB2. BRR (12.4 fixed) == fck/baud, OVER8=0 (default).
    USART1->BRR = SystemCoreClock / baud;
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

int hal_uart_getchar(uint32_t timeout_ms) {
    TickType_t start = xTaskGetTickCount();
    do {
        if (USART1->SR & USART_SR_RXNE) return (int)(USART1->DR & 0xFF);
        vTaskDelay(pdMS_TO_TICKS(1));
    } while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms));
    return -1;
}

void hal_uart_putchar(char c) {
    while (!(USART1->SR & USART_SR_TXE)) { }
    USART1->DR = (uint8_t)c;
}

void hal_uart_puts(const char *s) {
    while (*s) hal_uart_putchar(*s++);
}

// --- I2C (stub) ----------------------------------------------------------------

void hal_i2c_init(uint8_t, uint8_t, uint32_t) { /* TODO: I2C1 or bit-bang, like hal/stm32 */ }
bool hal_i2c_probe(uint8_t)                                   { return false; }
bool hal_i2c_write(uint8_t, uint8_t, const uint8_t *, size_t) { return false; }
bool hal_i2c_read (uint8_t, uint8_t,       uint8_t *, size_t) { return false; }
bool hal_i2c_read_raw(uint8_t,             uint8_t *, size_t) { return false; }
