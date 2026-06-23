#pragma once
#define HMC5883L_ADDR 0x1E

#include <stdint.h>
#include <math.h>
#include "hal/hal.h"
#include "core/IModule.h"
#include "core/CommandRegistry.h"
#include "i2c_ids.h"

// Grove 3-Axis Digital Compass v1.3 (HMC5883L).
// No platform dependencies — all I2C via hal.h.
class CompassModule : public IModule {
public:
    const char *name()  const override { return "compass"; }
    void        init()        override;
    void        registerCommands(CommandRegistry &reg) override;
    void        startTask()   override {}

    static bool readAxes(int16_t &x, int16_t &y, int16_t &z);
};

inline void CompassModule::init() {
    uint8_t mode = 0x00;  // continuous measurement
    hal_i2c_write(HMC5883L_ADDR, 0x02, &mode, 1);
}

inline bool CompassModule::readAxes(int16_t &x, int16_t &y, int16_t &z) {
    uint8_t buf[6];
    if (!hal_i2c_read(HMC5883L_ADDR, 0x03, buf, 6)) return false;
    // HMC5883L data order: X_H X_L Z_H Z_L Y_H Y_L
    x = (int16_t)((uint16_t)buf[0] << 8 | buf[1]);
    z = (int16_t)((uint16_t)buf[2] << 8 | buf[3]);
    y = (int16_t)((uint16_t)buf[4] << 8 | buf[5]);
    return true;
}

inline void CompassModule::registerCommands(CommandRegistry &reg) {
    reg.registerCommand(CMD("heading", "compass heading (0-359 deg)", CMD_COMPASS_HEADING,
        [](const char *, Writer &out, void *) {
            int16_t x; int16_t y; int16_t z;
            if (!CompassModule::readAxes(x, y, z)) { out.writeln("err"); return; }
            float hdg = atan2f((float)y, (float)x) * (180.0f / 3.14159265f);
            if (hdg < 0.0f) hdg += 360.0f;
            uint16_t deg = (uint16_t)hdg;
            char buf[8];
            uint8_t i = 0;
            if (deg >= 100) buf[i++] = '0' + deg / 100;
            if (deg >= 10)  buf[i++] = '0' + (deg / 10) % 10;
            buf[i++] = '0' + deg % 10;
            buf[i++] = ' '; buf[i++] = 'd'; buf[i++] = 'e'; buf[i++] = 'g';
            buf[i] = '\0';
            out.writeln(buf);
        }, nullptr));

    reg.registerCommand(CMD("raw", "raw X/Y/Z magnetometer counts", CMD_COMPASS_RAW,
        [](const char *, Writer &out, void *) {
            int16_t x; int16_t y; int16_t z;
            if (!CompassModule::readAxes(x, y, z)) { out.writeln("err"); return; }
            char buf[32];   // worst case "X=-32768 Y=-32768 Z=-32768" = 27 incl NUL
            uint8_t i = 0;
            auto appendI16 = [&](int16_t v) {
                if (v < 0) { buf[i++] = '-'; v = -v; }
                uint16_t u = (uint16_t)v;
                if (u >= 10000) buf[i++] = '0' + u / 10000;
                if (u >=  1000) buf[i++] = '0' + (u / 1000) % 10;
                if (u >=   100) buf[i++] = '0' + (u / 100)  % 10;
                if (u >=    10) buf[i++] = '0' + (u / 10)   % 10;
                buf[i++] = '0' + u % 10;
            };
            buf[i++] = 'X'; buf[i++] = '='; appendI16(x);
            buf[i++] = ' '; buf[i++] = 'Y'; buf[i++] = '='; appendI16(y);
            buf[i++] = ' '; buf[i++] = 'Z'; buf[i++] = '='; appendI16(z);
            buf[i] = '\0';
            out.writeln(buf);
        }, nullptr));
}
