#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- I2C -----------------------------------------------------------------
// Call hal_i2c_init() once from platform main before registering modules.
void hal_i2c_init (uint8_t sda_pin, uint8_t scl_pin, uint32_t speed_hz);
bool hal_i2c_probe(uint8_t addr);  // true if a device ACKs at addr
bool hal_i2c_write(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len);
bool hal_i2c_read (uint8_t addr, uint8_t reg,       uint8_t *data, size_t len);

// --- GPIO ----------------------------------------------------------------
void     hal_gpio_set_output(uint8_t pin);
void     hal_gpio_set_input (uint8_t pin);
void     hal_gpio_write     (uint8_t pin, bool high);
bool     hal_gpio_read      (uint8_t pin);

// Returns pulse duration in microseconds, 0 on timeout.
uint32_t hal_pulse_in_us(uint8_t pin, bool level, uint32_t timeout_us);

// --- Time ----------------------------------------------------------------
void     hal_delay_ms(uint32_t ms);
uint64_t hal_time_us (void);

// --- UART ----------------------------------------------------------------
// hal_uart_init() must be called before the FreeRTOS scheduler starts.
void hal_uart_init   (uint32_t baud);
int  hal_uart_getchar(uint32_t timeout_ms);  // returns char or -1 on timeout
void hal_uart_putchar(char c);
void hal_uart_puts   (const char *s);

#ifdef __cplusplus
}
#endif
