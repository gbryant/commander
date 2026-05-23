#pragma once
#include "modules/ir/IIRModule.h"
#include "core/CommandRegistry.h"
#include "include/i2c_ids.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/sync.h"  // __dmb()
#include "pico/multicore.h"
#include "FreeRTOS.h"
#include "task.h"
#include "ir_rx.pio.h"
#include "hal/hal.h"
#include <stdio.h>

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

    explicit PicoIRModule(uint gpio) : _gpio(gpio) {}

    const char *name() const override { return "ir"; }

    void init() override {
        // Configure PIO but leave SM disabled until launch() — active PIO during
        // WiFi WPA2 handshake can cause BADAUTH (-7).
        _pio        = pio1;  // pio0 is reserved for the CYW43 SPI bus
        _sm         = pio_claim_unused_sm(_pio, true);
        uint offset = pio_add_program(_pio, &ir_rx_program);
        ir_rx_program_init(_pio, _sm, offset, _gpio);
        pio_sm_set_enabled(_pio, _sm, false);
        gpio_pull_up(_gpio);
    }

    void launch() {
        s_instance = this;
        pio_sm_set_enabled(_pio, _sm, true);
        multicore_launch_core1(core1Entry);
    }

    void registerCommands(CommandRegistry &reg) override {
        reg.registerCommand(CMD("ir diag", "count PIO events and decoded codes over 5 s", I2C_NONE,
            [](const char *, Writer &out, void *ctx) {
                auto *m = static_cast<PicoIRModule *>(ctx);
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

        reg.registerCommand(CMD("recv", "toggle IR receive mode (NEC/Sony)", CMD_IR_RECV,
            [](const char *, Writer &out, void *ctx) {
                auto *m = static_cast<PicoIRModule *>(ctx);
                m->_active = !m->_active;
                if (m->_active) {
                    m->ringDrain();
                    out.writeln("listening... (recv to stop)");
                } else {
                    out.writeln("stopped.");
                }
            }, this));
    }

    // Called from UartTransport task on core0; outputs decoded codes when active.
    void tick() override {
        if (!_active) return;
        if (_ring.head == _ring.tail) return;

        uint8_t  proto = _ring.proto[_ring.head];
        uint8_t  nbits = _ring.nbits[_ring.head];
        uint32_t code  = _ring.code [_ring.head];
        __dmb();
        _ring.head = (_ring.head + 1) % RING_CAP;

        char buf[96];
        if (proto == PROTO_NEC) {
            // Bit k of raw = k-th received bit, so:
            //   bits[7:0]   = addr_low  (1st byte received)
            //   bits[15:8]  = addr_high (2nd byte received)
            //   bits[23:16] = command   (3rd byte received)
            //   bits[31:24] = ~command  (4th byte received)
            // Extended NEC: addr_high != ~addr_low → 16-bit address matches IRremote.
            uint8_t addr_low  = (uint8_t)(code & 0xFF);
            uint8_t addr_high = (uint8_t)((code >> 8) & 0xFF);
            uint8_t cmd       = (uint8_t)((code >> 16) & 0xFF);
            bool    extended  = (addr_high != (uint8_t)~addr_low);
            if (extended) {
                uint16_t addr16 = (uint16_t)((addr_high << 8) | addr_low);
                snprintf(buf, sizeof(buf),
                         "Protocol=NEC  Address=0x%X,  Command=0x%X,"
                         "  Raw-Data=0x%08X,  32 bits\r\n",
                         addr16, cmd, (unsigned)code);
            } else {
                snprintf(buf, sizeof(buf),
                         "Protocol=NEC  Address=0x%X,  Command=0x%X,"
                         "  Raw-Data=0x%08X,  32 bits\r\n",
                         (unsigned)addr_low, cmd, (unsigned)code);
            }
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
                continue;
            }
            decode(us, is_mark);
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

    uint8_t  _gpio;
    PIO      _pio  = nullptr;
    uint     _sm   = 0;

    bool             _active   = false;
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

    static PicoIRModule *s_instance;
};

inline PicoIRModule *PicoIRModule::s_instance = nullptr;
