#pragma once
#include "modules/ir/IIRModule.h"
#include "core/CommandRegistry.h"
#include "include/i2c_ids.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/multicore.h"
#include "FreeRTOS.h"
#include "task.h"
#include "ir_rx.pio.h"
#include <stdio.h>

// IR receive runs on core1 (bare-metal spin loop).
// Decoded codes are pushed through the SIO inter-core FIFO as two entries:
// protocol (PROTO_NEC or PROTO_SONY) then the 32-bit code.
class PicoIRModule : public IIRModule {
public:
    static constexpr uint8_t PROTO_NEC  = 1;
    static constexpr uint8_t PROTO_SONY = 2;

    explicit PicoIRModule(uint gpio) : _gpio(gpio) {}

    const char *name() const override { return "ir"; }

    void init() override {
        // PIO setup only. multicore_launch_core1 must be called via launch()
        // after WiFi connects — core1 spinning before the WPA2 handshake
        // causes bus contention that corrupts auth timing.
        _pio        = pio1;  // pio0 is reserved for the CYW43 SPI bus
        _sm         = pio_claim_unused_sm(_pio, true);
        uint offset = pio_add_program(_pio, &ir_rx_program);
        ir_rx_program_init(_pio, _sm, offset, _gpio);
    }

    void launch() {
        s_instance = this;
        multicore_launch_core1(core1Entry);
    }

    void registerCommands(CommandRegistry &reg) override {
        reg.registerCommand(CMD("ir diag", "count PIO events and decoded codes over 5 s", I2C_NONE,
            [](const char *, Writer &out, void *ctx) {
                auto *m = static_cast<PicoIRModule *>(ctx);
                while (multicore_fifo_rvalid()) {
                    multicore_fifo_pop_blocking();
                    if (multicore_fifo_rvalid()) multicore_fifo_pop_blocking();
                }
                m->_pio_events = 0;
                m->_code_count = 0;
                char pin_buf[32];
                snprintf(pin_buf, sizeof(pin_buf), "GP%u = %d (expect 1 when idle)",
                         (unsigned)m->_gpio, (int)gpio_get(m->_gpio));
                out.writeln(pin_buf);
                out.writeln("press remote buttons for 5 s...");
                vTaskDelay(pdMS_TO_TICKS(5000));
                char buf[48];
                snprintf(buf, sizeof(buf), "pio_events=%lu  codes=%lu",
                         (unsigned long)m->_pio_events,
                         (unsigned long)m->_code_count);
                out.writeln(buf);
            }, this));
        reg.registerCommand(CMD("ir recv", "wait for NEC/Sony IR code (10 s timeout)", CMD_IR_RECV,
            [](const char *, Writer &out, void *ctx) {
                auto *m = static_cast<PicoIRModule *>(ctx);
                // Drain stale codes (in pairs)
                while (multicore_fifo_rvalid()) {
                    multicore_fifo_pop_blocking();
                    if (multicore_fifo_rvalid()) multicore_fifo_pop_blocking();
                }
                m->_available = false;
                for (int i = 0; i < 100; i++) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                    if (multicore_fifo_rvalid()) {
                        uint8_t  proto = (uint8_t)multicore_fifo_pop_blocking();
                        uint32_t code  = multicore_fifo_pop_blocking();
                        char buf[32];
                        snprintf(buf, sizeof(buf), "0x%08X  %s",
                                 (unsigned)code,
                                 proto == PROTO_NEC ? "NEC" : "Sony");
                        out.writeln(buf);
                        return;
                    }
                }
                out.writeln("timeout");
            }, this));
    }

    // IIRModule — polled from core0 only
    bool dataAvailable() const override {
        if (!_available && multicore_fifo_rvalid()) {
            _protocol  = (uint8_t)multicore_fifo_pop_blocking();
            _code      = multicore_fifo_pop_blocking();
            _available = true;
        }
        return _available;
    }
    uint32_t getCode()     const override { return _code; }
    uint8_t  getProtocol() const override { return _protocol; }

private:
    static void core1Entry() { s_instance->core1Loop(); }

    // Runs on core1 — free to spin; never touches FreeRTOS or CYW43.
    // PIO pushes alternating mark/space values; _expect_mark tracks which is next.
    // Long durations (>15 ms) are idle gaps — resync state.
    void core1Loop() {
        for (;;) {
            if (pio_sm_is_rx_fifo_empty(_pio, _sm)) continue;
            _pio_events++;
            uint32_t raw = pio_sm_get(_pio, _sm);
            uint32_t us  = (uint32_t)(-(int32_t)raw) * 3;
            bool is_mark = _expect_mark;
            _expect_mark = !_expect_mark;
            if (us > 15000) {           // idle gap — resync
                _expect_mark = true;
                _state       = IDLE;
                continue;
            }
            decode(us, is_mark);
        }
    }

    // NEC:  leader mark ~9000 µs, leader space ~4500 µs
    //       bit mark ~562 µs, bit-0 space ~562 µs, bit-1 space ~1688 µs
    //       32 bits, LSB first: addr | ~addr<<8 | cmd<<16 | ~cmd<<24
    //
    // Sony: leader mark ~2400 µs, leader space ~600 µs
    //       bit-0 mark ~600 µs, bit-1 mark ~1200 µs, all spaces ~600 µs
    //       12/15/20 bits, LSB first: cmd[6:0] | addr<<7
    void decode(uint32_t us, bool is_mark) {
        switch (_state) {
        case IDLE:
            if (!is_mark) break;
            if (us > 7000)              { _state = NEC_SPACE;  break; }
            if (us > 1800)              { _state = SONY_SPACE; break; }
            break;

        case NEC_SPACE:
            if (is_mark) { _state = IDLE; break; }
            if (us > 3500)              { _state = NEC_DATA; _shift = 0; _bits = 0; }
            else                        { _state = IDLE; }
            break;

        case NEC_DATA:
            if (is_mark) break;         // bit marks are uniform — ignore
            _shift >>= 1;
            if (us > 1000) _shift |= 0x80000000u;
            if (++_bits == 32)          { emit(PROTO_NEC, _shift); _state = IDLE; }
            break;

        case SONY_SPACE:
            if (is_mark) { _state = IDLE; break; }
            if (us < 1200)              { _state = SONY_DATA; _shift = 0; _bits = 0; }
            else                        { _state = IDLE; }
            break;

        case SONY_DATA:
            if (!is_mark) break;        // bit spaces are uniform — ignore
            _shift >>= 1;
            if (us > 900) _shift |= 0x80000000u;
            ++_bits;
            // Sony frames end with idle (detected above as >15 ms).
            // Emit when we reach a known bit count.
            if (_bits == 12 || _bits == 15 || _bits == 20) {
                // Shift down to align: bits arrived LSB-first into bit 31
                uint32_t code = _shift >> (32 - _bits);
                emit(PROTO_SONY, code);
                _state = IDLE;
            }
            break;
        }
    }

    void emit(uint8_t protocol, uint32_t code) {
        _code_count++;
        multicore_fifo_push_blocking(protocol);
        multicore_fifo_push_blocking(code);
    }

    uint8_t  _gpio;
    PIO      _pio  = nullptr;
    uint     _sm   = 0;

    // Accessed only from core0
    mutable bool     _available = false;
    mutable uint32_t _code      = 0;
    mutable uint8_t  _protocol  = 0;

    // Written by core1, read by core0 (approximate — no sync needed for diagnostics)
    volatile uint32_t _pio_events = 0;
    volatile uint32_t _code_count = 0;

    // Accessed only from core1
    enum State { IDLE, NEC_SPACE, NEC_DATA, SONY_SPACE, SONY_DATA };
    State    _state       = IDLE;
    bool     _expect_mark = true;
    uint32_t _shift       = 0;
    uint8_t  _bits        = 0;

    static PicoIRModule *s_instance;
};

inline PicoIRModule *PicoIRModule::s_instance = nullptr;
