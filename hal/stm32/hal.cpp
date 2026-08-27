// STM32F103 native HAL — implements hal/hal.h against CMSIS registers (no vendor HAL).
//
// Pin encoding: a pin is (port << 4) | bit, where port 0..3 = GPIOA..GPIOD.
//   PA0 = 0x00, PA9 = 0x09, PB6 = 0x16, PC13 = 0x2D, ...
//
// Time base: the Cortex-M3 DWT cycle counter (free-running at SystemCoreClock).
// I2C: stubbed until Phase 4 (compass is the only consumer).
#include "../hal.h"
#include "stm32f1xx.h"
#include "FreeRTOS.h"
#include "task.h"

// --- helpers -----------------------------------------------------------------

static GPIO_TypeDef *port_of(uint8_t pin) {
    switch (pin >> 4) {
        case 0:  return GPIOA;
        case 1:  return GPIOB;
        case 2:  return GPIOC;
        default: return GPIOD;
    }
}

static void enable_port_clock(uint8_t pin) {
    switch (pin >> 4) {
        case 0:  RCC->APB2ENR |= RCC_APB2ENR_IOPAEN; break;
        case 1:  RCC->APB2ENR |= RCC_APB2ENR_IOPBEN; break;
        case 2:  RCC->APB2ENR |= RCC_APB2ENR_IOPCEN; break;
        default: RCC->APB2ENR |= RCC_APB2ENR_IOPDEN; break;
    }
}

// Write the 4-bit MODE/CNF nibble for one pin into CRL (0-7) or CRH (8-15).
static void set_pin_cfg(uint8_t pin, uint32_t cfg4) {
    GPIO_TypeDef *g = port_of(pin);
    uint8_t bit = pin & 0xF;
    if (bit < 8) {
        uint32_t sh = bit * 4;
        g->CRL = (g->CRL & ~(0xFu << sh)) | (cfg4 << sh);
    } else {
        uint32_t sh = (bit - 8) * 4;
        g->CRH = (g->CRH & ~(0xFu << sh)) | (cfg4 << sh);
    }
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

void hal_gpio_set_output(uint8_t pin) { enable_port_clock(pin); set_pin_cfg(pin, 0x2); } // PP, 2MHz
void hal_gpio_set_input (uint8_t pin) { enable_port_clock(pin); set_pin_cfg(pin, 0x4); } // floating in

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
    uint32_t cyc_per_us  = SystemCoreClock / 1000000UL;     // 72
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

// 64-bit µs clock built from the 32-bit DWT counter (wraps every ~59 s at 72 MHz),
// extended by accumulating deltas. Not reentrant — only the UART task uses it.
uint64_t hal_time_us(void) {
    static uint64_t accum = 0;
    static uint32_t last  = 0;
    dwt_init();
    uint32_t now = DWT->CYCCNT;
    accum += (uint32_t)(now - last);   // unsigned subtraction handles one wrap
    last   = now;
    return accum / (SystemCoreClock / 1000000UL);
}

// --- UART console ------------------------------------------------------------
// Two interchangeable backends behind the same hal_uart_* surface, so UartTransport
// is identical either way. Default: USART1. With -DCOMMANDER_STM32_USB_CONSOLE: USB CDC.

#ifndef COMMANDER_STM32_USB_CONSOLE

// USART1: PA9 = TX, PA10 = RX.
void hal_uart_init(uint32_t baud) {
    dwt_init();
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_AFIOEN | RCC_APB2ENR_USART1EN;
    // PA9  = USART1_TX: alternate-function push-pull, 50 MHz  -> CNF=10 MODE=11 = 0xB
    GPIOA->CRH = (GPIOA->CRH & ~(0xFu << 4)) | (0xBu << 4);
    // PA10 = USART1_RX: input floating                        -> 0x4
    GPIOA->CRH = (GPIOA->CRH & ~(0xFu << 8)) | (0x4u << 8);
    // USART1 is on APB2 (= SYSCLK, PPRE2 = /1). BRR (12.4 fixed) == fck/baud.
    USART1->BRR = SystemCoreClock / baud;
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

// Poll RXNE, yielding the task each ms so lower-priority tasks run (mirrors the
// Arduino HAL's delay(1)). Only called from the UART task, after the scheduler is up.
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

#else  // COMMANDER_STM32_USB_CONSOLE

#include "tusb.h"

// USB is brought up by usb_hw_init() + the usbd task (platform/stm32-bluepill/usb.c);
// the CDC FIFOs are mutex-protected by TinyUSB's FreeRTOS OSAL, so it's safe for this
// (the UART task) to read/write while the usbd task pumps tud_task().
void hal_uart_init(uint32_t /*baud*/) { /* nothing — USB host negotiates the rate */ }

int hal_uart_getchar(uint32_t timeout_ms) {
    TickType_t start = xTaskGetTickCount();
    do {
        if (tud_cdc_available()) return (int)tud_cdc_read_char();
        vTaskDelay(pdMS_TO_TICKS(1));
    } while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms));
    return -1;
}

void hal_uart_putchar(char c) {
    tud_cdc_write_char(c);
    tud_cdc_write_flush();
}

void hal_uart_puts(const char *s) {
    tud_cdc_write_str(s);
    tud_cdc_write_flush();
}

#endif  // COMMANDER_STM32_USB_CONSOLE

// --- I2C (Phase 4) -----------------------------------------------------------

void hal_i2c_init(uint8_t, uint8_t, uint32_t) { /* TODO Phase 4: I2C1 or bit-bang */ }
bool hal_i2c_probe(uint8_t)                                   { return false; }
bool hal_i2c_write(uint8_t, uint8_t, const uint8_t *, size_t) { return false; }
bool hal_i2c_read (uint8_t, uint8_t,       uint8_t *, size_t) { return false; }
bool hal_i2c_read_raw(uint8_t,             uint8_t *, size_t) { return false; }
bool hal_i2c_write_read(uint8_t, const uint8_t *, size_t, uint8_t *, size_t) { return false; }

// --- SPI / ADC / PWM ---------------------------------------------------------
// Not implemented on this platform yet. The modules that need them (st7796,
// gt911's bus is fine but joystick/buzzer/display are not) are gated to the
// platforms whose HAL backs them, via `platforms` in MODULE_SPECS
// (tools/cmdr/src/cmdr/cli.py) — so nothing can enable a peripheral this HAL
// can't drive. Implement these and widen that list in the same change.
void hal_spi_init(uint8_t, int8_t, int8_t, int8_t, uint32_t) {}
void hal_spi_set_speed(uint8_t, uint32_t)                    {}
void hal_spi_write(uint8_t, const uint8_t *, size_t)         {}
void hal_spi_write16(uint8_t, const uint16_t *, size_t)      {}
void hal_spi_transfer(uint8_t, const uint8_t *, uint8_t *, size_t) {}
int8_t   hal_adc_init(uint8_t)     { return -1; }
uint16_t hal_adc_read(uint8_t)     { return 0; }
uint16_t hal_adc_max (void)        { return 0; }
void hal_pwm_init(uint8_t)         {}
void hal_pwm_duty(uint8_t, uint8_t)   {}
void hal_pwm_tone(uint8_t, uint32_t, uint8_t) {}
void hal_pwm_stop(uint8_t)            {}
