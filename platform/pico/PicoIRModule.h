#pragma once
#include "modules/ir/IIRModule.h"
#include "modules/ir/IrEvent.h"     // ir_nec_split — the shared NEC address/command split
#include "core/CommandRegistry.h"
#include "include/i2c_ids.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/sync.h"  // __dmb()
#include "pico/multicore.h"
#include "FreeRTOS.h"
#include "task.h"
#include "hal/hal.h"
#include <stdio.h>
// NOTE: the generated ir_rx.pio.h is included only by PicoIRModule.cpp, so this
// header stays clean — consumers (e.g. cmdr's commander_modules.h) include it
// without any PIO build wiring. PicoIRModule.cpp is compiled by the
// commander_pico_ir CMake target, which owns the pico_generate_pio_header call.

// IR receive runs on core1 (bare-metal spin loop).
// Decoded codes are passed to core0 via a lock-free SPSC ring buffer in shared SRAM.
//
// NOTE: The RP2040 FreeRTOS port installs SIO_IRQ_PROC0 which drains the
// inter-core SIO FIFO (multicore_fifo_*) automatically for its yield mechanism.
// Using multicore_fifo_push from core1 causes FreeRTOS to silently consume every
// code before ir_recv can see it (pushed=N, rvalid=always false). The ring buffer
// lives in ordinary SRAM and is invisible to FreeRTOS.
class PicoIRModule : public IIRModule {
public:
    static constexpr uint8_t PROTO_NEC  = 1;
    static constexpr uint8_t PROTO_SONY = 2;
    static constexpr uint8_t PROTO_WALL = 3;  // Roomba virtual wall

    explicit PicoIRModule(uint gpio) : _gpio(gpio) {}

    const char *name() const override { return "ir"; }

    // Configure PIO (leaves the SM disabled until launch() — active PIO during
    // the WiFi WPA2 handshake can cause BADAUTH). Defined in PicoIRModule.cpp,
    // which owns the generated ir_rx.pio.h include.
    void init() override;

    // Enable the PIO SM and launch the core1 receive loop. Idempotent and lazy:
    // called on first use of an IR command (recv / ir diag), by which point WiFi
    // is connected — so no explicit post-WiFi hook is needed.
    void launch() {
        if (_launched) return;
        _launched   = true;
        s_instance  = this;
        pio_sm_set_enabled(_pio, _sm, true);
        multicore_launch_core1(core1Entry);
    }

    void registerCommands(CommandRegistry &reg) override {
        reg.registerCommand(CMD("ir diag", "count PIO events and decoded codes over 5 s", I2C_NONE,
            [](const char *, Writer &out, void *ctx) {
                auto *m = static_cast<PicoIRModule *>(ctx);
                m->launch();
                m->ringDrain();
                m->_pio_events = 0;
                m->_code_count = 0;
                m->_push_count = 0;
                m->_drop_count = 0;
                char pin_buf[32];
                snprintf(pin_buf, sizeof(pin_buf), "GP%u = %d (expect 1 when idle)",
                         (unsigned)m->_gpio, (int)gpio_get(m->_gpio));
                out.writeln(pin_buf);
                out.writeln("press remote buttons for 5 s...");
                vTaskDelay(pdMS_TO_TICKS(5000));
                char buf[80];
                snprintf(buf, sizeof(buf), "pio_events=%lu  codes=%lu  pushed=%lu  dropped=%lu",
                         (unsigned long)m->_pio_events,
                         (unsigned long)m->_code_count,
                         (unsigned long)m->_push_count,
                         (unsigned long)m->_drop_count);
                out.writeln(buf);
                m->ringDrain();
            }, this));

        reg.registerCommand(CMD("ir recv", "toggle IR receive mode (NEC/Sony)", CMD_IR_RECV,
            [](const char *, Writer &out, void *ctx) {
                auto *m = static_cast<PicoIRModule *>(ctx);
                m->launch();
                m->_wallMode = false;
                m->_active   = !m->_active;
                if (m->_active) {
                    m->ringDrain();
                    out.writeln("listening... (ir recv to stop)");
                } else {
                    out.writeln("stopped.");
                }
            }, this));

#ifdef COMMANDER_IR_WALL
        reg.registerCommand(CMD("ir wall", "detect Roomba virtual wall transmissions", CMD_IR_WALL,
            [](const char *, Writer &out, void *ctx) {
                auto *m = static_cast<PicoIRModule *>(ctx);
                m->launch();
                m->_active   = false;
                m->_wallMode = !m->_wallMode;
                if (m->_wallMode) {
                    m->ringDrain();
                    out.writeln("watching for Roomba wall signals... (ir wall to stop)");
                } else {
                    out.writeln("stopped.");
                }
            }, this));
#endif // COMMANDER_IR_WALL
    }

    // Called from UartTransport task on core0; outputs decoded codes when active.
    void tick() override {
        if (!_active && !_wallMode) return;
        if (_ring.head == _ring.tail) return;

        uint8_t  proto = _ring.proto[_ring.head];
        uint8_t  nbits = _ring.nbits[_ring.head];
        uint32_t code  = _ring.code [_ring.head];
        __dmb();
        _ring.head = (_ring.head + 1) % RING_CAP;

        if (proto == PROTO_WALL) {
            if (_wallMode) hal_uart_puts("\r\n[Roomba] Virtual Wall (0xA5)\r\n");
            return;
        }
        if (!_active) return;   // NEC/Sony code while in wall mode — ignore

        char buf[96];
        if (proto == PROTO_NEC) {
            // Standard vs extended NEC addressing lives in ir_nec_split() — one implementation
            // for every platform, so a map built on one board matches presses on another.
            uint32_t addr; uint32_t cmd;
            ir_nec_split(code, &addr, &cmd);
            snprintf(buf, sizeof(buf),
                     "Protocol=NEC  Address=0x%X,  Command=0x%X,"
                     "  Raw-Data=0x%08X,  32 bits\r\n",
                     (unsigned)addr, (unsigned)cmd, (unsigned)code);
        } else {
            // Sony SIRC: 7-bit command (LSB first, bits[6:0]), then address bits.
            uint8_t  cmd  = (uint8_t)(code & 0x7F);
            uint32_t addr = (code >> 7) & ((1u << (nbits - 7)) - 1u);
            snprintf(buf, sizeof(buf),
                     "Protocol=Sony  Address=0x%X,  Command=0x%X,"
                     "  Raw-Data=0x%08X,  %u bits\r\n",
                     (unsigned)addr, cmd, (unsigned)code, nbits);
        }
        hal_uart_puts(buf);
    }

    // IIRModule — polled from core0 only
    bool dataAvailable() const override {
        if (!_available && _ring.head != _ring.tail) {
            _protocol  = _ring.proto[_ring.head];
            _code      = _ring.code [_ring.head];
            __dmb();
            _ring.head = (_ring.head + 1) % RING_CAP;
            _available = true;
        }
        return _available;
    }
    uint32_t getCode()     const override { return _code; }
    uint8_t  getProtocol() const override { return _protocol; }

private:
    // SPSC ring buffer: core1 writes tail, core0 reads head.
    // __dmb() between data write and index update guarantees ordering on Cortex-M0+.
    static constexpr uint8_t RING_CAP = 8;
    struct Ring {
        volatile uint32_t head = 0;
        volatile uint32_t tail = 0;
        volatile uint8_t  proto[RING_CAP] = {};
        volatile uint8_t  nbits[RING_CAP] = {};
        volatile uint32_t code [RING_CAP] = {};
    };
    mutable Ring _ring;

    void ringDrain() { _ring.head = _ring.tail; }

    static void core1Entry() { s_instance->core1Loop(); }

    void core1Loop() {
        for (;;) {
            if (pio_sm_is_rx_fifo_empty(_pio, _sm)) continue;
            _pio_events++;
            uint32_t raw = pio_sm_get(_pio, _sm);
            uint32_t us  = (uint32_t)(-(int32_t)raw) * 3;
            bool is_mark = _expect_mark;
            _expect_mark = !_expect_mark;
            if (us > 15000) {
                _expect_mark = true;
                _state       = IDLE;
                _wallMarks   = 0;
                continue;
            }
            decode(us, is_mark);
#ifdef COMMANDER_IR_WALL
            wallTrack(us, is_mark);
#endif
        }
    }

    // NEC:  leader mark ~9000 µs, leader space ~4500 µs
    //       bit mark ~562 µs, bit-0 space ~562 µs, bit-1 space ~1688 µs
    // Sony: leader mark ~2400 µs, leader space ~600 µs
    //       bit-0 mark ~600 µs, bit-1 mark ~1200 µs
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
            if (is_mark) break;
            _shift >>= 1;
            if (us > 1000) _shift |= 0x80000000u;
            if (++_bits == 32)          { emit(PROTO_NEC, 32, _shift); _state = IDLE; }
            break;
        case SONY_SPACE:
            if (is_mark) { _state = IDLE; break; }
            if (us < 1200)              { _state = SONY_DATA; _shift = 0; _bits = 0; }
            else                        { _state = IDLE; }
            break;
        case SONY_DATA:
            if (!is_mark) break;
            _shift >>= 1;
            if (us > 900) _shift |= 0x80000000u;
            ++_bits;
            if (_bits == 12 || _bits == 15 || _bits == 20) {
                emit(PROTO_SONY, _bits, _shift >> (32 - _bits));
                _state = IDLE;
            }
            break;
        }
    }

    void emit(uint8_t protocol, uint8_t nbits, uint32_t code) {
        _code_count++;
        uint32_t next = (_ring.tail + 1) % RING_CAP;
        if (next == _ring.head) { _drop_count++; return; }
        _ring.proto[_ring.tail] = protocol;
        _ring.nbits[_ring.tail] = nbits;
        _ring.code [_ring.tail] = code;
        __dmb();
        _ring.tail = next;
        _push_count++;
    }

#ifdef COMMANDER_IR_WALL
    // Roomba virtual wall: repeating ~550us mark + ~7350us space bursts. Detected
    // in parallel with the NEC/Sony state machine — the 550us marks are too short
    // to trigger NEC/Sony, so the two don't interfere. Emits PROTO_WALL after 3
    // valid bursts. NOTE: timing window is approximate; tune against hardware.
    void wallTrack(uint32_t us, bool is_mark) {
        if (is_mark) {
            if (us > 360 && us < 760) {            // ~550us mark
                if (++_wallMarks >= 3) { emit(PROTO_WALL, 0, 0xA5); _wallMarks = 0; }
            } else {
                _wallMarks = 0;
            }
        } else if (!(us > 4700 && us < 10000)) {   // space must be ~7350us
            _wallMarks = 0;
        }
    }
#endif // COMMANDER_IR_WALL

    uint8_t  _gpio;
    PIO      _pio  = nullptr;
    uint     _sm   = 0;
    bool     _launched = false;

    bool             _active   = false;
    bool             _wallMode = false;
    mutable bool     _available = false;
    mutable uint32_t _code      = 0;
    mutable uint8_t  _protocol  = 0;

    volatile uint32_t _pio_events = 0;
    volatile uint32_t _code_count = 0;
    volatile uint32_t _push_count = 0;
    volatile uint32_t _drop_count = 0;

    enum State { IDLE, NEC_SPACE, NEC_DATA, SONY_SPACE, SONY_DATA };
    State    _state       = IDLE;
    bool     _expect_mark = true;
    uint32_t _shift       = 0;
    uint8_t  _bits        = 0;
    uint8_t  _wallMarks   = 0;   // consecutive ~550us wall marks seen (core1)

    static PicoIRModule *s_instance;
};

inline PicoIRModule *PicoIRModule::s_instance = nullptr;
