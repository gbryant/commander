#pragma once
#include "hal/hal.h"
#include "core/IModule.h"
#include "core/CommandRegistry.h"
#include "i2c_ids.h"

class I2cModule : public IModule {
public:
    const char *name()  const override { return "i2c"; }
    void        init()        override {}
    void        registerCommands(CommandRegistry &reg) override;
};

inline void I2cModule::registerCommands(CommandRegistry &reg) {
    reg.registerCommand(CMD("scan", "scan I2C bus (0x08-0x77)", CMD_I2C_SCAN,
        [](const char *, Writer &out, void *) {
            static const char hex[] = "0123456789ABCDEF";
            uint8_t found = 0;
            for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
                hal_delay_ms(1);
                if (hal_i2c_probe(addr)) {
                    char buf[5];
                    buf[0] = '0'; buf[1] = 'x';
                    buf[2] = hex[addr >> 4];
                    buf[3] = hex[addr & 0x0F];
                    buf[4] = '\0';
                    out.writeln(buf);
                    found++;
                }
            }
            char buf[12];
            if (found == 0) {
                out.writeln("none");
            } else {
                uint8_t i = 0;
                if (found >= 10) buf[i++] = '0' + found / 10;
                buf[i++] = '0' + found % 10;
                buf[i++] = ' '; buf[i++] = 'f'; buf[i++] = 'o';
                buf[i++] = 'u'; buf[i++] = 'n'; buf[i++] = 'd';
                buf[i] = '\0';
                out.writeln(buf);
            }
        }, nullptr));
}
