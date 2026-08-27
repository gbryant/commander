#include "../hal.h"
#include "hardware/i2c.h"
#include "hardware/spi.h"
#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include "pico/stdio.h"
#include "FreeRTOS.h"
#include "task.h"
#include <cstdio>
#include <string.h>

static i2c_inst_t *_i2c_bus = i2c0;

void hal_i2c_init(uint8_t sda_pin, uint8_t scl_pin, uint32_t speed_hz) {
    // Pick the controller the pins are wired to: on RP2040/RP2350 the I2C block
    // alternates with each GPIO pair, so bit 1 of the SDA pin selects i2c0/i2c1
    // (GP0/4/8.. = i2c0, GP2/6/10.. = i2c1). gpio_set_function then routes the
    // pin to that block's fixed I2C function — so e.g. GP6/GP7 must use i2c1.
    _i2c_bus = (sda_pin & 2) ? i2c1 : i2c0;
    i2c_init(_i2c_bus, speed_hz);
    gpio_set_function(sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(scl_pin, GPIO_FUNC_I2C);
    gpio_pull_up(sda_pin);
    gpio_pull_up(scl_pin);
}

bool hal_i2c_probe(uint8_t addr) {
    uint8_t dummy;
    return i2c_read_blocking_until(_i2c_bus, addr, &dummy, 1, false,
                                   make_timeout_time_ms(10)) >= 0;
}

bool hal_i2c_write(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len) {
    uint8_t buf[len + 1];
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    return i2c_write_blocking_until(_i2c_bus, addr, buf, len + 1, false,
                                    make_timeout_time_ms(10)) == (int)(len + 1);
}

bool hal_i2c_read(uint8_t addr, uint8_t reg, uint8_t *data, size_t len) {
    if (i2c_write_blocking_until(_i2c_bus, addr, &reg, 1, true,
                                 make_timeout_time_ms(10)) != 1) return false;
    return i2c_read_blocking_until(_i2c_bus, addr, data, len, false,
                                   make_timeout_time_ms(10)) == (int)len;
}

bool hal_i2c_read_raw(uint8_t addr, uint8_t *data, size_t len) {
    return i2c_read_blocking_until(_i2c_bus, addr, data, len, false,
                                   make_timeout_time_ms(10)) == (int)len;
}

bool hal_i2c_write_read(uint8_t addr, const uint8_t *wdata, size_t wlen,
                        uint8_t *rdata, size_t rlen) {
    // Hold the bus (nostop) ONLY when a read follows — that's the repeated START
    // multi-byte-register parts (GT911) require. With no read to follow, the
    // write must be terminated with a STOP or the transaction is never completed:
    // the target sees an unfinished write and may ignore it. That bit us for
    // real — the GT911's "touch consumed" acknowledgement is a write with no
    // read, and leaving it open made the controller re-report a stale touch
    // until further touches shook it loose.
    const bool hold = (rlen != 0);
    if (i2c_write_blocking_until(_i2c_bus, addr, wdata, wlen, hold,
                                 make_timeout_time_ms(10)) != (int)wlen) return false;
    if (rlen == 0) return true;
    return i2c_read_blocking_until(_i2c_bus, addr, rdata, rlen, false,
                                   make_timeout_time_ms(10)) == (int)rlen;
}

void hal_gpio_set_output(uint8_t pin) { gpio_init(pin); gpio_set_dir(pin, GPIO_OUT); }
void hal_gpio_set_input (uint8_t pin) { gpio_init(pin); gpio_set_dir(pin, GPIO_IN);  }
void hal_gpio_write(uint8_t pin, bool high) { gpio_put(pin, high); }
bool hal_gpio_read (uint8_t pin) { return gpio_get(pin); }

uint32_t hal_pulse_in_us(uint8_t pin, bool level, uint32_t timeout_us) {
    uint64_t t0 = time_us_64();
    while ((bool)gpio_get(pin) != level) {
        if (time_us_64() - t0 > timeout_us) return 0;
    }
    uint64_t pulse_start = time_us_64();
    while ((bool)gpio_get(pin) == level) {
        if (time_us_64() - pulse_start > timeout_us) return 0;
    }
    return (uint32_t)(time_us_64() - pulse_start);
}

// --- SPI ---------------------------------------------------------------------
// RP2040/RP2350 have two SPI controllers. Which one a pin belongs to is fixed by
// the pinmux, so the caller passes the bus its wiring dictates (e.g. the Pico
// Breadboard Kit's panel is GP2/GP3 = spi0).
static spi_inst_t *spi_of(uint8_t bus) { return bus ? spi1 : spi0; }

void hal_spi_init(uint8_t bus, int8_t sck_pin, int8_t mosi_pin, int8_t miso_pin,
                  uint32_t speed_hz) {
    spi_init(spi_of(bus), speed_hz);
    spi_set_format(spi_of(bus), 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    if (sck_pin  >= 0) gpio_set_function((uint)sck_pin,  GPIO_FUNC_SPI);
    if (mosi_pin >= 0) gpio_set_function((uint)mosi_pin, GPIO_FUNC_SPI);
    if (miso_pin >= 0) gpio_set_function((uint)miso_pin, GPIO_FUNC_SPI);
}

void hal_spi_set_speed(uint8_t bus, uint32_t speed_hz) {
    spi_set_baudrate(spi_of(bus), speed_hz);
}

void hal_spi_write(uint8_t bus, const uint8_t *data, size_t len) {
    spi_write_blocking(spi_of(bus), data, len);
}

void hal_spi_write16(uint8_t bus, const uint16_t *data, size_t count) {
    // Switch the frame width rather than byte-swapping in software: the SPI
    // block shifts a 16-bit frame MSB-first, which is exactly RGB565 wire order.
    spi_inst_t *s = spi_of(bus);
    spi_set_format(s, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    spi_write16_blocking(s, data, count);
    spi_set_format(s, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
}

void hal_spi_transfer(uint8_t bus, const uint8_t *tx, uint8_t *rx, size_t len) {
    spi_inst_t *s = spi_of(bus);
    if (tx && rx)      spi_write_read_blocking(s, tx, rx, len);
    else if (tx)       spi_write_blocking(s, tx, len);
    else if (rx)       spi_read_blocking(s, 0x00, rx, len);
}

// --- ADC ---------------------------------------------------------------------
static bool _adc_ready = false;

int8_t hal_adc_init(uint8_t pin) {
    if (pin < 26 || pin > 29) return -1;      // not an ADC-capable GPIO
    if (!_adc_ready) { adc_init(); _adc_ready = true; }
    adc_gpio_init(pin);
    return (int8_t)(pin - 26);                // GP26..GP29 → channels 0..3
}

uint16_t hal_adc_read(uint8_t channel) {
    adc_select_input(channel);
    return adc_read();           // 12-bit
}

uint16_t hal_adc_max(void) { return 4095; }

// --- PWM ---------------------------------------------------------------------
// Duty mode uses a fixed 8-bit wrap so hal_pwm_duty is a plain level write; tone
// mode reprograms wrap+divider for the requested frequency at 50% duty. Both
// share the slice, so a pin can move between them (backlight → beep → backlight).
void hal_pwm_init(uint8_t pin) {
    gpio_set_function(pin, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(pin);
    pwm_config c = pwm_get_default_config();
    pwm_config_set_wrap(&c, 255);
    pwm_init(slice, &c, true);
    pwm_set_gpio_level(pin, 0);
}

void hal_pwm_duty(uint8_t pin, uint8_t duty) {
    gpio_set_function(pin, GPIO_FUNC_PWM);   // hal_pwm_stop hands the pin back to SIO
    uint slice = pwm_gpio_to_slice_num(pin);
    pwm_set_clkdiv(slice, 4.0f);         // ~122 kHz at 125 MHz sys clock — silent
    pwm_set_wrap(slice, 255);
    pwm_set_gpio_level(pin, duty);
    pwm_set_enabled(slice, true);
}

void hal_pwm_tone(uint8_t pin, uint32_t freq_hz) {
    if (freq_hz == 0) { hal_pwm_stop(pin); return; }
    gpio_set_function(pin, GPIO_FUNC_PWM);   // hal_pwm_stop hands the pin back to SIO
    uint slice = pwm_gpio_to_slice_num(pin);
    // Pick the smallest divider that keeps wrap inside 16 bits, so low notes and
    // high notes both land close to the requested pitch.
    uint32_t sys = clock_get_hz(clk_sys);
    float div = (float)sys / (freq_hz * 65536.0f);
    if (div < 1.0f) div = 1.0f;
    uint32_t wrap = (uint32_t)((float)sys / (div * freq_hz)) - 1;
    if (wrap > 65535) wrap = 65535;
    pwm_set_clkdiv(slice, div);
    pwm_set_wrap(slice, (uint16_t)wrap);
    pwm_set_gpio_level(pin, (uint16_t)(wrap / 2));   // 50% duty = square wave
    pwm_set_enabled(slice, true);
}

void hal_pwm_stop(uint8_t pin) {
    // Disabling a slice FREEZES its output at whatever level the counter left it
    // at — and pwm_set_gpio_level() only takes effect at the next wrap, which
    // never comes once the slice is off. So stopping this way leaves the pin high
    // about half the time. On a buzzer that is a continuous tone that nothing can
    // clear (hardware, 2026-08-27: intermittent stuck wail from every input path,
    // with the buzzer module's own start/stop counts perfectly balanced).
    //
    // Take the pin back as a plain output and drive it low, so "stopped" is a
    // state we set rather than one we hope the counter left behind.
    pwm_set_gpio_level(pin, 0);
    pwm_set_enabled(pwm_gpio_to_slice_num(pin), false);
    gpio_set_function(pin, GPIO_FUNC_SIO);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, false);
}

void     hal_delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
uint64_t hal_time_us(void)         { return time_us_64(); }

// stdio_init_all() in main.cpp connects USB-CDC (or UART) to stdin/stdout.
void hal_uart_init(uint32_t /*baud*/) {}  // handled by pico_enable_stdio_usb in CMake
int  hal_uart_getchar(uint32_t timeout_ms) {
    int c = getchar_timeout_us(0);
    if (c != PICO_ERROR_TIMEOUT) return c;
    // Yield via vTaskDelay so lower-priority tasks (WiFi, cyw43 async) get CPU.
    // getchar_timeout_us busy-polls and would starve them if called directly.
    if (timeout_ms > 0) vTaskDelay(pdMS_TO_TICKS(timeout_ms));
    c = getchar_timeout_us(0);
    return (c == PICO_ERROR_TIMEOUT) ? -1 : c;
}
void hal_uart_putchar(char c)     { putchar(c); fflush(stdout); }
void hal_uart_puts(const char *s) { fputs(s, stdout); fflush(stdout); }
