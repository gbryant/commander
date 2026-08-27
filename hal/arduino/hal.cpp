#include "../hal.h"
#include <Arduino.h>
#include <Wire.h>

void hal_i2c_init(uint8_t sda_pin, uint8_t scl_pin, uint32_t speed_hz) {
    Wire.begin();
    Wire.setClock(speed_hz);
}

bool hal_i2c_probe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

bool hal_i2c_write(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    for (size_t i = 0; i < len; i++) Wire.write(data[i]);
    return Wire.endTransmission() == 0;
}

bool hal_i2c_read(uint8_t addr, uint8_t reg, uint8_t *data, size_t len) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom(addr, (uint8_t)len);
    if ((size_t)Wire.available() < len) return false;
    for (size_t i = 0; i < len; i++) data[i] = Wire.read();
    return true;
}

bool hal_i2c_read_raw(uint8_t addr, uint8_t *data, size_t len) {
    Wire.requestFrom(addr, (uint8_t)len);
    if ((size_t)Wire.available() < len) return false;
    for (size_t i = 0; i < len; i++) data[i] = Wire.read();
    return true;
}

bool hal_i2c_write_read(uint8_t addr, const uint8_t *wdata, size_t wlen,
                        uint8_t *rdata, size_t rlen) {
    Wire.beginTransmission(addr);
    for (size_t i = 0; i < wlen; i++) Wire.write(wdata[i]);
    if (Wire.endTransmission(false) != 0) return false;   // false = repeated start
    if (rlen == 0) return true;
    Wire.requestFrom(addr, (uint8_t)rlen);
    if ((size_t)Wire.available() < rlen) return false;
    for (size_t i = 0; i < rlen; i++) rdata[i] = Wire.read();
    return true;
}

void hal_gpio_set_output(uint8_t pin) { pinMode(pin, OUTPUT); }
void hal_gpio_set_input (uint8_t pin) { pinMode(pin, INPUT);  }
void hal_gpio_write(uint8_t pin, bool high) { digitalWrite(pin, high ? HIGH : LOW); }
bool hal_gpio_read (uint8_t pin) { return digitalRead(pin) != LOW; }

uint32_t hal_pulse_in_us(uint8_t pin, bool level, uint32_t timeout_us) {
    return (uint32_t)pulseIn(pin, level ? HIGH : LOW, (unsigned long)timeout_us);
}

void     hal_delay_ms(uint32_t ms)  { delay(ms); }

// micros() is 32-bit and wraps every ~70 min. Detect rollovers by comparing
// successive calls; requires call frequency > 1/70 min (trivially met by the
// UART task). Not ISR-safe, but hal_time_us is only called from task context.
static uint32_t _last_us = 0;
static uint64_t _high    = 0;
uint64_t hal_time_us(void) {
    uint32_t now = micros();
    if (now < _last_us) _high += (uint64_t)1 << 32;
    _last_us = now;
    return _high | now;
}

void hal_uart_init(uint32_t baud) { Serial.begin(baud); }

// delay() in feilipu FreeRTOS calls vTaskDelay — yields the UART task each
// millisecond so lower-priority tasks (idle, loop) actually get to run.
int hal_uart_getchar(uint32_t timeout_ms) {
    uint32_t start = millis();
    do {
        if (Serial.available()) return Serial.read();
        delay(1);
    } while (millis() - start < timeout_ms);
    return -1;
}

void hal_uart_putchar(char c)     { Serial.write(c); }
void hal_uart_puts(const char *s) { Serial.print(s); }

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
