#pragma once
#include <stdint.h>
#include "hal/hal.h"
#include "core/IModule.h"
#include "core/CommandRegistry.h"
#include "i2c_ids.h"

// RadioShack 276-0342 / Parallax PING-style: single-pin trigger + echo.
// Pin is configured at construction; platform main passes the board pin number.
class SonarModule : public IModule {
public:
    explicit SonarModule(uint8_t pin) : _pin(pin) {}

    const char *name()  const override { return "sonar"; }
    void        init()        override { hal_gpio_set_input(_pin); }
    void        registerCommands(CommandRegistry &reg) override;
    void        startTask()   override {}

private:
    uint8_t _pin;
};

inline void SonarModule::registerCommands(CommandRegistry &reg) {
    reg.registerCommand(CMD("ping", "measure distance (cm / in)", CMD_SONAR_PING,
        [](const char *, Writer &out, void *ctx) {
            uint8_t pin = *static_cast<uint8_t *>(ctx);

            hal_gpio_set_output(pin);
            hal_gpio_write(pin, false);
            // 2 µs low, 5 µs high trigger pulse
            uint64_t t = hal_time_us();
            while (hal_time_us() - t < 2) {}
            hal_gpio_write(pin, true);
            t = hal_time_us();
            while (hal_time_us() - t < 5) {}
            hal_gpio_write(pin, false);

            hal_gpio_set_input(pin);
            uint32_t us = hal_pulse_in_us(pin, true, 30000);

            if (us == 0) { out.writeln("no echo"); return; }

            uint16_t cm     = (uint16_t)(us / 58);
            uint16_t inches = (uint16_t)(us / 148);

            char buf[16];
            uint8_t i = 0;
            auto appendU16 = [&](uint16_t v) {
                if (v >= 100) buf[i++] = '0' + v / 100;
                if (v >= 10)  buf[i++] = '0' + (v / 10) % 10;
                buf[i++] = '0' + v % 10;
            };
            appendU16(cm);
            buf[i++]=' '; buf[i++]='c'; buf[i++]='m';
            buf[i++]=' '; buf[i++]='/'; buf[i++]=' ';
            appendU16(inches);
            buf[i++]=' '; buf[i++]='i'; buf[i++]='n';
            buf[i] = '\0';
            out.writeln(buf);
        }, &_pin));
}
