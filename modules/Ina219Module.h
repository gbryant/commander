#pragma once
#include <stdint.h>
#include "hal/hal.h"
#include "core/IModule.h"
#include "core/CommandRegistry.h"
#include "i2c_ids.h"

// Texas Instruments INA219 current/power monitor via I²C.
// Calibrated for a 0.1 Ω shunt (Adafruit / SparkFun standard breakout).
//   Current_LSB = 100 µA   →  current register × 0.1 = mA
//   Power_LSB   = 2 mW     →  power   register × 2   = mW
//
// Multiple instances require distinct prefixes to avoid command-name collision:
//   Ina219Module ina_a(0x40, "a"), ina_b(0x41, "b");
//   → commands: avolt  aamp  awatt  bvolt  bamp  bwatt
class Ina219Module : public IModule {
public:
    explicit Ina219Module(uint8_t addr, const char *prefix) : _addr(addr) {
        cat(_volt_cmd, sizeof(_volt_cmd), prefix, "volt");
        cat(_amp_cmd,  sizeof(_amp_cmd),  prefix, "amp");
        cat(_watt_cmd, sizeof(_watt_cmd), prefix, "watt");
    }

    const char *name()  const override { return _volt_cmd; }
    void        init()        override;
    void        registerCommands(CommandRegistry &reg) override;
    void        startTask()   override {}

private:
    uint8_t _addr;
    char    _volt_cmd[12];
    char    _amp_cmd[12];
    char    _watt_cmd[12];

    static void cat(char *dst, size_t n, const char *a, const char *b) {
        size_t i = 0;
        while (*a && i < n - 1) dst[i++] = *a++;
        while (*b && i < n - 1) dst[i++] = *b++;
        dst[i] = '\0';
    }

    static bool readReg(uint8_t addr, uint8_t reg, uint16_t &out) {
        uint8_t buf[2];
        if (!hal_i2c_read(addr, reg, buf, 2)) return false;
        out = (uint16_t)((uint16_t)buf[0] << 8 | buf[1]);
        return true;
    }

    static void cmdVolt(const char *, Writer &out, void *ctx);
    static void cmdAmp (const char *, Writer &out, void *ctx);
    static void cmdWatt(const char *, Writer &out, void *ctx);
};

inline void Ina219Module::init() {
    // Configuration: 32 V bus range, ÷8 PGA (±320 mV shunt), 12-bit ADC, continuous.
    // Matches the power-on default; written anyway as an explicit reset.
    uint8_t cfg[2] = {0x39, 0x9F};
    hal_i2c_write(_addr, 0x00, cfg, 2);
    // Cal = 4096 (0x1000) for 0.1 Ω shunt → Current_LSB = 100 µA.
    uint8_t cal[2] = {0x10, 0x00};
    hal_i2c_write(_addr, 0x05, cal, 2);
}

inline void Ina219Module::cmdVolt(const char *, Writer &out, void *ctx) {
    auto *m = static_cast<Ina219Module *>(ctx);
    uint16_t raw;
    if (!readReg(m->_addr, 0x02, raw)) { out.writeln("err"); return; }
    // Bits [15:3] hold voltage; 1 LSB = 4 mV.
    uint32_t mV = (uint32_t)(raw >> 3) * 4;
    char buf[12]; uint8_t i = 0;
    if (mV >= 10000) buf[i++] = '0' + (char)(mV / 10000);
    if (mV >=  1000) buf[i++] = '0' + (char)((mV /  1000) % 10);
    if (mV >=   100) buf[i++] = '0' + (char)((mV /   100) % 10);
    if (mV >=    10) buf[i++] = '0' + (char)((mV /    10) % 10);
    buf[i++] = '0' + (char)(mV % 10);
    buf[i++] = ' '; buf[i++] = 'm'; buf[i++] = 'V'; buf[i] = '\0';
    out.writeln(buf);
}

inline void Ina219Module::cmdAmp(const char *, Writer &out, void *ctx) {
    auto *m = static_cast<Ina219Module *>(ctx);
    uint16_t raw;
    if (!readReg(m->_addr, 0x04, raw)) { out.writeln("err"); return; }
    // Current register is signed; 1 LSB = 100 µA = 0.1 mA.
    int16_t cur = (int16_t)raw;
    char buf[16]; uint8_t i = 0;
    if (cur < 0) {
        buf[i++] = '-';
        cur = (cur == (int16_t)-32768) ? (int16_t)32767 : (int16_t)-cur;
    }
    uint16_t u = (uint16_t)cur;
    uint16_t mA = u / 10; uint8_t frac = (uint8_t)(u % 10);
    if (mA >= 1000) buf[i++] = '0' + (char)(mA / 1000);
    if (mA >=  100) buf[i++] = '0' + (char)((mA /  100) % 10);
    if (mA >=   10) buf[i++] = '0' + (char)((mA /   10) % 10);
    buf[i++] = '0' + (char)(mA % 10);
    buf[i++] = '.'; buf[i++] = '0' + frac;
    buf[i++] = ' '; buf[i++] = 'm'; buf[i++] = 'A'; buf[i] = '\0';
    out.writeln(buf);
}

inline void Ina219Module::cmdWatt(const char *, Writer &out, void *ctx) {
    auto *m = static_cast<Ina219Module *>(ctx);
    uint16_t raw;
    if (!readReg(m->_addr, 0x03, raw)) { out.writeln("err"); return; }
    // 1 LSB = 20 × Current_LSB = 20 × 100 µA = 2 mW.
    uint32_t mW = (uint32_t)raw * 2;
    char buf[12]; uint8_t i = 0;
    if (mW >= 100000) buf[i++] = '0' + (char)(mW / 100000);
    if (mW >=  10000) buf[i++] = '0' + (char)((mW /  10000) % 10);
    if (mW >=   1000) buf[i++] = '0' + (char)((mW /   1000) % 10);
    if (mW >=    100) buf[i++] = '0' + (char)((mW /    100) % 10);
    if (mW >=     10) buf[i++] = '0' + (char)((mW /     10) % 10);
    buf[i++] = '0' + (char)(mW % 10);
    buf[i++] = ' '; buf[i++] = 'm'; buf[i++] = 'W'; buf[i] = '\0';
    out.writeln(buf);
}

inline void Ina219Module::registerCommands(CommandRegistry &reg) {
    // I2C_NONE: these are local sensor reads, not I²C relay commands.
    // Two instances share the same handler code but differ by _addr via ctx.
    reg.registerCommand(CMD(_volt_cmd, "bus voltage (mV)", I2C_NONE, cmdVolt, this));
    reg.registerCommand(CMD(_amp_cmd,  "current (mA)",     I2C_NONE, cmdAmp,  this));
    reg.registerCommand(CMD(_watt_cmd, "power (mW)",       I2C_NONE, cmdWatt, this));
}
