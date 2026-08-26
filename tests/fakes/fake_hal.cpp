#include "fake_hal.h"
#include "hal/hal.h"
#include <stdio.h>

namespace fake_hal {

std::vector<Event>   log;
uint16_t             adc_values[8] = {2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048};
std::vector<bool>    gpio_levels(64, false);
std::vector<uint8_t> i2c_response;
std::map<uint16_t, std::vector<uint8_t>> i2c_regs;
bool                 i2c_ack = true;
uint64_t             now_us  = 0;

void reset() {
    log.clear();
    for (uint16_t &v : adc_values) v = 2048;
    gpio_levels.assign(64, false);
    i2c_response.clear();
    i2c_regs.clear();
    i2c_ack = true;
    now_us  = 0;
}

void setAdc(int channel, uint16_t value) {
    if (channel >= 0 && channel < 8) adc_values[channel] = value;
}

void setGpio(int pin, bool level) {
    if (pin >= 0 && (size_t)pin < gpio_levels.size()) gpio_levels[(size_t)pin] = level;
}

std::vector<uint8_t> spiWith(int dc_pin, bool dc_level) {
    std::vector<uint8_t> out;
    bool dc = false;
    for (const Event &e : log) {
        if (e.kind == Event::GpioWrite && e.pin == dc_pin) dc = e.value != 0;
        else if (e.kind == Event::SpiWrite && dc == dc_level)
            out.insert(out.end(), e.bytes.begin(), e.bytes.end());
    }
    return out;
}

std::vector<uint16_t> pixels() {
    std::vector<uint16_t> out;
    for (const Event &e : log)
        if (e.kind == Event::SpiWrite16)
            for (size_t i = 0; i + 1 < e.bytes.size(); i += 2)
                out.push_back((uint16_t)((e.bytes[i] << 8) | e.bytes[i + 1]));
    return out;
}

size_t count(Event::Kind k) {
    size_t n = 0;
    for (const Event &e : log) if (e.kind == k) n++;
    return n;
}

std::string dump() {
    static const char *kNames[] = {
        "GpioOut", "GpioIn", "GpioWrite", "SpiInit", "SpiWrite", "SpiWrite16", "SpiSpeed",
        "I2cInit", "I2cWrite", "I2cRead", "I2cWriteRead", "AdcInit", "AdcRead",
        "PwmInit", "PwmDuty", "PwmTone", "PwmStop", "Delay"};
    std::string s;
    char line[128];
    for (const Event &e : log) {
        snprintf(line, sizeof(line), "%-12s pin=%-3d val=%-8u n=%zu\n",
                 kNames[e.kind], e.pin, e.value, e.bytes.size());
        s += line;
    }
    return s;
}

static void push(Event::Kind k, int pin = -1, uint32_t value = 0,
                 const uint8_t *data = nullptr, size_t len = 0) {
    Event e;
    e.kind = k; e.pin = pin; e.value = value;
    if (data && len) e.bytes.assign(data, data + len);
    log.push_back(std::move(e));
}

}  // namespace fake_hal

using namespace fake_hal;

// ── hal/hal.h implementation ────────────────────────────────────────────────
extern "C" {

void hal_i2c_init(uint8_t sda, uint8_t scl, uint32_t hz) { push(Event::I2cInit, sda, hz); (void)scl; }
bool hal_i2c_probe(uint8_t addr) { push(Event::I2cInit, addr); return i2c_ack; }

bool hal_i2c_write(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len) {
    std::vector<uint8_t> b; b.push_back(reg);
    b.insert(b.end(), data, data + len);
    push(Event::I2cWrite, addr, 0, b.data(), b.size());
    return i2c_ack;
}

bool hal_i2c_read(uint8_t addr, uint8_t reg, uint8_t *data, size_t len) {
    push(Event::I2cRead, addr, reg);
    if (!i2c_ack) return false;
    for (size_t i = 0; i < len; i++)
        data[i] = i < i2c_response.size() ? i2c_response[i] : 0;
    return true;
}

bool hal_i2c_read_raw(uint8_t addr, uint8_t *data, size_t len) {
    push(Event::I2cRead, addr);
    if (!i2c_ack) return false;
    for (size_t i = 0; i < len; i++)
        data[i] = i < i2c_response.size() ? i2c_response[i] : 0;
    return true;
}

bool hal_i2c_write_read(uint8_t addr, const uint8_t *wdata, size_t wlen,
                        uint8_t *rdata, size_t rlen) {
    push(Event::I2cWriteRead, addr, (uint32_t)rlen, wdata, wlen);
    if (!i2c_ack) return false;
    const std::vector<uint8_t> *src = &i2c_response;
    if (wlen >= 2) {
        uint16_t reg = (uint16_t)((wdata[0] << 8) | wdata[1]);
        auto it = i2c_regs.find(reg);
        if (it != i2c_regs.end()) src = &it->second;
    }
    for (size_t i = 0; i < rlen; i++)
        rdata[i] = i < src->size() ? (*src)[i] : 0;
    return true;
}

void hal_gpio_set_output(uint8_t pin) { push(Event::GpioOut, pin); }
void hal_gpio_set_input (uint8_t pin) { push(Event::GpioIn,  pin); }
void hal_gpio_write(uint8_t pin, bool high) { push(Event::GpioWrite, pin, high ? 1 : 0); }
bool hal_gpio_read (uint8_t pin) {
    return pin < gpio_levels.size() ? gpio_levels[pin] : false;
}
uint32_t hal_pulse_in_us(uint8_t, bool, uint32_t) { return 0; }

void hal_spi_init(uint8_t bus, int8_t, int8_t, int8_t, uint32_t hz) { push(Event::SpiInit, bus, hz); }
void hal_spi_set_speed(uint8_t bus, uint32_t hz) { push(Event::SpiSpeed, bus, hz); }
void hal_spi_write(uint8_t bus, const uint8_t *data, size_t len) {
    push(Event::SpiWrite, bus, (uint32_t)len, data, len);
}
void hal_spi_write16(uint8_t bus, const uint16_t *data, size_t count) {
    // Recorded MSB-first, matching what the peripheral shifts out in 16-bit mode.
    std::vector<uint8_t> b;
    b.reserve(count * 2);
    for (size_t i = 0; i < count; i++) {
        b.push_back((uint8_t)(data[i] >> 8));
        b.push_back((uint8_t)(data[i] & 0xFF));
    }
    push(Event::SpiWrite16, bus, (uint32_t)count, b.data(), b.size());
}
void hal_spi_transfer(uint8_t bus, const uint8_t *tx, uint8_t *rx, size_t len) {
    push(Event::SpiWrite, bus, (uint32_t)len, tx, tx ? len : 0);
    if (rx) for (size_t i = 0; i < len; i++)
        rx[i] = i < i2c_response.size() ? i2c_response[i] : 0;
}

int8_t   hal_adc_init(uint8_t pin) { push(Event::AdcInit, pin);
                                    return (pin >= 26 && pin <= 29) ? (int8_t)(pin - 26) : -1; }
uint16_t hal_adc_read(uint8_t ch)  {
    uint16_t v = ch < 8 ? adc_values[ch] : 0;
    push(Event::AdcRead, ch, v);
    return v;
}
uint16_t hal_adc_max (void)        { return 4095; }

void hal_pwm_init(uint8_t pin)              { push(Event::PwmInit, pin); }
void hal_pwm_duty(uint8_t pin, uint8_t d)   { push(Event::PwmDuty, pin, d); }
void hal_pwm_tone(uint8_t pin, uint32_t hz) { push(Event::PwmTone, pin, hz); }
void hal_pwm_stop(uint8_t pin)              { push(Event::PwmStop, pin); }

void     hal_delay_ms(uint32_t ms) { push(Event::Delay, -1, ms); now_us += (uint64_t)ms * 1000; }
uint64_t hal_time_us(void)         { return now_us; }

void hal_uart_init(uint32_t)     {}
int  hal_uart_getchar(uint32_t)  { return -1; }
void hal_uart_putchar(char)      {}
void hal_uart_puts(const char *) {}

}  // extern "C"
