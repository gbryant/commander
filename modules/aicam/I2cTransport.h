#pragma once
#include <stdint.h>
#include "hal/hal.h"
#include "modules/aicam/Sscma.h"

// Portable I2C backend for the SSCMA transport seam (Grove Vision AI V2 @ 0x62).
// Ports Seeed_Arduino_SSCMA's I2C framing onto the HAL: every transaction is a
// 4-byte header { feature, cmd, len_hi, len_lo } followed by an optional payload
// and a 2-byte (currently zero) checksum. Reads issue a command write then a
// separate register-less read (hal_i2c_read_raw) — the device serves the bytes.
//
// hal_i2c_write(0x62, FEATURE_TRANSPORT, {cmd, len_hi, len_lo, ...payload..., 0,0})
// puts FEATURE_TRANSPORT on the wire as the first byte (the HAL's `reg` arg), so
// the on-wire layout matches the reference exactly.
//
// Call hal_i2c_init(sda, scl, 400000) before use (the generated commander_modules.h
// does this when the i2c transport is selected).
class AiCamI2cTransport : public ISscmaTransport {
public:
    static constexpr uint8_t kDefaultAddr = 0x62;

    explicit AiCamI2cTransport(uint8_t addr = kDefaultAddr, uint32_t waitMs = 2)
        : _addr(addr), _wait(waitMs) {}

    void txWrite(const char *data, int len) override {
        const uint8_t *d = (const uint8_t *)data;
        int off = 0;
        while (off < len) {
            int n = len - off;
            if (n > MAX_PL) n = MAX_PL;
            writeFrame(CMD_WRITE, d + off, n);
            off += n;
        }
    }

    int rxAvailable() override {
        hal_delay_ms(_wait);
        uint8_t hdr[HEADER_LEN + CHECKSUM_LEN] = { FEATURE_TRANSPORT, CMD_AVAILABLE, 0, 0, 0, 0 };
        // hdr[0] is the HAL `reg` byte; the rest is the payload.
        if (!hal_i2c_write(_addr, hdr[0], hdr + 1, sizeof(hdr) - 1)) return 0;
        hal_delay_ms(_wait);
        uint8_t buf[2] = {0, 0};
        if (!hal_i2c_read_raw(_addr, buf, 2)) return 0;
        return ((int)buf[0] << 8) | buf[1];
    }

    int rxRead(char *data, int len) override {
        int off = 0;
        while (off < len) {
            int n = len - off;
            if (n > MAX_PL) n = MAX_PL;
            requestRead(n);
            hal_delay_ms(_wait);
            if (!hal_i2c_read_raw(_addr, (uint8_t *)data + off, n)) break;
            off += n;
        }
        return off;
    }

private:
    // Seeed SSCMA I2C transport constants.
    static constexpr uint8_t  FEATURE_TRANSPORT = 0x10;
    static constexpr uint8_t  CMD_READ          = 0x01;
    static constexpr uint8_t  CMD_WRITE         = 0x02;
    static constexpr uint8_t  CMD_AVAILABLE     = 0x03;
    static constexpr uint8_t  HEADER_LEN        = 4;
    static constexpr uint8_t  CHECKSUM_LEN      = 2;
    static constexpr int      MAX_PL            = 250;

    uint8_t  _addr;
    uint32_t _wait;

    void writeFrame(uint8_t cmd, const uint8_t *payload, int n) {
        hal_delay_ms(_wait);
        uint8_t buf[HEADER_LEN - 1 + MAX_PL + CHECKSUM_LEN];
        // buf[] is the payload after the FEATURE_TRANSPORT byte (the HAL `reg`).
        int i = 0;
        buf[i++] = cmd;
        buf[i++] = (uint8_t)(n >> 8);
        buf[i++] = (uint8_t)(n & 0xFF);
        for (int j = 0; j < n; j++) buf[i++] = payload[j];
        buf[i++] = 0;   // checksum hi (unused by firmware)
        buf[i++] = 0;   // checksum lo
        hal_i2c_write(_addr, FEATURE_TRANSPORT, buf, i);
    }

    void requestRead(int n) {
        hal_delay_ms(_wait);
        uint8_t buf[HEADER_LEN - 1 + CHECKSUM_LEN] = {
            CMD_READ, (uint8_t)(n >> 8), (uint8_t)(n & 0xFF), 0, 0
        };
        hal_i2c_write(_addr, FEATURE_TRANSPORT, buf, sizeof(buf));
    }
};
