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

void hal_gpio_set_output(uint8_t pin) { pinMode(pin, OUTPUT); }
void hal_gpio_set_input (uint8_t pin) { pinMode(pin, INPUT);  }
void hal_gpio_write(uint8_t pin, bool high) { digitalWrite(pin, high ? HIGH : LOW); }
bool hal_gpio_read (uint8_t pin) { return digitalRead(pin) != LOW; }

uint32_t hal_pulse_in_us(uint8_t pin, bool level, uint32_t timeout_us) {
    return (uint32_t)pulseIn(pin, level ? HIGH : LOW, (unsigned long)timeout_us);
}

void     hal_delay_ms(uint32_t ms)  { delay(ms); }
// micros() wraps at ~70 min; fine for pulse timing, not for long uptimes
uint64_t hal_time_us(void) { return (uint64_t)micros(); }

void hal_uart_init(uint32_t baud)          { Serial.begin(baud); }
int  hal_uart_getchar(uint32_t /*timeout*/) { return Serial.available() ? Serial.read() : -1; }
void hal_uart_putchar(char c)              { Serial.write(c); }
void hal_uart_puts(const char *s)          { Serial.print(s); }
