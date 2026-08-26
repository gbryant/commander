#pragma once
#include <stdint.h>
#include <map>
#include <string>
#include <vector>

// ── Recording HAL for host tests ─────────────────────────────────────────────
// Implements the whole hal/hal.h C interface in terms of an in-memory log, so
// peripheral drivers (display, touch, joystick, buzzer…) can be exercised on a
// laptop: drive the module, then assert on the exact bytes it put on the wire.
//
// This is the only way most of this code gets verified before the hardware is on
// the desk — and it stays useful afterwards, because a logic bug looks the same
// here as it does on the bench, but takes a second to find.
namespace fake_hal {

struct Event {
    enum Kind { GpioOut, GpioIn, GpioWrite, SpiInit, SpiWrite, SpiWrite16, SpiSpeed,
                I2cInit, I2cWrite, I2cRead, I2cWriteRead, AdcInit, AdcRead,
                PwmInit, PwmDuty, PwmTone, PwmStop, Delay };
    Kind                 kind;
    int                  pin   = -1;    // pin / bus / channel, per kind
    uint32_t             value = 0;     // level, frequency, duty, speed, ms…
    std::vector<uint8_t> bytes;         // SPI/I2C payload (16-bit writes: MSB first)
};

// The log every fake_* function appends to.
extern std::vector<Event> log;

// Test-controlled inputs.
extern uint16_t              adc_values[8];  // per-channel hal_adc_read results
extern std::vector<bool>     gpio_levels;    // hal_gpio_read, indexed by pin
extern std::vector<uint8_t>  i2c_response;   // bytes handed back by reads
// Per-register canned answers for 16-bit-addressed parts (GT911): keyed by the
// register the driver asked for. Falls back to i2c_response when a key is absent.
extern std::map<uint16_t, std::vector<uint8_t>> i2c_regs;
extern bool                  i2c_ack;        // false = every transaction fails
extern uint64_t              now_us;         // hal_time_us, advanced by hal_delay_ms

void reset();
void setGpio(int pin, bool level);
void setAdc(int channel, uint16_t value);

// ── Assertions/queries used by the tests ─────────────────────────────────────
// Bytes written while `dc_pin` was low (commands) or high (data), in order.
std::vector<uint8_t> spiWith(int dc_pin, bool dc_level);
// Every 16-bit word pushed (pixels), in order.
std::vector<uint16_t> pixels();
// Count of events of a kind.
size_t count(Event::Kind k);
// Human-readable dump for failure messages.
std::string dump();

}  // namespace fake_hal
