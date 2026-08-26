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
// Register-less read: a pure read transaction (no preceding register write), the
// equivalent of Arduino's requestFrom(). Needed by transports (e.g. SSCMA/aicam)
// whose protocol issues a separate command write then reads N raw bytes back.
bool hal_i2c_read_raw(uint8_t addr, uint8_t *data, size_t len);
// Combined write→read in ONE transaction (repeated start, no stop between). Needed
// by devices with multi-byte register addresses — e.g. the GT911 touch panel
// addresses 16-bit registers (0x814E), which hal_i2c_read's single uint8_t reg
// cannot express, and which will not answer a write-STOP-read sequence.
bool hal_i2c_write_read(uint8_t addr, const uint8_t *wdata, size_t wlen,
                                            uint8_t *rdata, size_t rlen);

// --- GPIO ----------------------------------------------------------------
void     hal_gpio_set_output(uint8_t pin);
void     hal_gpio_set_input (uint8_t pin);
void     hal_gpio_write     (uint8_t pin, bool high);
bool     hal_gpio_read      (uint8_t pin);

// Returns pulse duration in microseconds, 0 on timeout.
uint32_t hal_pulse_in_us(uint8_t pin, bool level, uint32_t timeout_us);

// --- SPI ----------------------------------------------------------------
// Master mode, mode 0 (CPOL=0/CPHA=0), MSB first. `bus` is 0 or 1 — which
// controller a pin set belongs to is fixed by the chip, so pass the bus the
// wiring dictates. Chip-select is deliberately NOT handled here: panels and
// sensors differ in CS/DC timing, so callers drive those with hal_gpio_write().
// Pass miso < 0 for a write-only device (most display panels).
void hal_spi_init     (uint8_t bus, int8_t sck_pin, int8_t mosi_pin, int8_t miso_pin,
                       uint32_t speed_hz);
void hal_spi_set_speed(uint8_t bus, uint32_t speed_hz);
void hal_spi_write    (uint8_t bus, const uint8_t *data, size_t len);
// 16-bit frames, sent MSB-first on the wire. Pixel pushes (RGB565) use this so
// the driver never has to byte-swap a framebuffer — the peripheral does it.
void hal_spi_write16  (uint8_t bus, const uint16_t *data, size_t count);
// Full-duplex: rx may be null (write-only), tx may be null (clocks out zeros).
void hal_spi_transfer (uint8_t bus, const uint8_t *tx, uint8_t *rx, size_t len);

// --- ADC ----------------------------------------------------------------
// hal_adc_init() prepares one pin for analog input and RETURNS ITS CHANNEL, or
// -1 if the pin has no ADC. Returning the channel is what keeps analog modules
// portable: pin→channel differs per chip (GP26..GP29 → 0..3 on RP2040/RP2350),
// and a module that hardcoded that arithmetic would be a Pico module.
// hal_adc_read() then takes the channel, so sampling several inputs is cheap.
// Reads are right-aligned raw counts in the converter's native width
// (12-bit / 0..4095 on RP2040 and RP2350) — scale with hal_adc_max().
int8_t   hal_adc_init(uint8_t pin);
uint16_t hal_adc_read(uint8_t channel);
uint16_t hal_adc_max (void);   // full-scale count, e.g. 4095 — for portable scaling

// --- PWM ----------------------------------------------------------------
// Two uses, one peripheral: proportional output (backlight, motor) via
// hal_pwm_duty(), and audio via hal_pwm_tone() (fixed 50% duty at a frequency).
void hal_pwm_init(uint8_t pin);
void hal_pwm_duty(uint8_t pin, uint8_t duty);       // 0..255
void hal_pwm_tone(uint8_t pin, uint32_t freq_hz);   // 0 Hz == stop
void hal_pwm_stop(uint8_t pin);

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
