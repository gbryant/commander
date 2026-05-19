#include "../hal.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include "pico/stdio.h"
#include "FreeRTOS.h"
#include "task.h"
#include <cstdio>
#include <string.h>

static i2c_inst_t *_i2c_bus = i2c0;

void hal_i2c_init(uint8_t sda_pin, uint8_t scl_pin, uint32_t speed_hz) {
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
