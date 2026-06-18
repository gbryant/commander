#pragma once
#include "modules/ir/IIRModule.h"
#include "modules/ir/NecDecoder.h"
#include "modules/ir/SonyDecoder.h"
#include "core/CommandRegistry.h"
#include "core/Writer.h"
#include <stdint.h>

// IR receive for ESP32 via the RMT peripheral (1 µs resolution, hardware-timed).
// The RMT on_recv_done callback fires in ISR context when a frame completes (idle
// gap > signal_range_max_ns). It decodes the symbol stream through NecDecoder /
// SonyDecoder and pushes results into a SPSC ring; tick() drains and prints.
// Header is RMT-free so consumers include it without the esp-idf driver headers.
class Esp32IRModule : public IIRModule {
public:
    static constexpr uint8_t PROTO_NEC  = 1;
    static constexpr uint8_t PROTO_SONY = 2;
    static constexpr uint8_t PROTO_WALL = 3;

    explicit Esp32IRModule(int gpio) : _gpio(gpio) {}

    const char *name() const override { return "ir"; }
    void        init()       override;
    void        registerCommands(CommandRegistry &reg) override;
    void        tick()       override;

    bool     dataAvailable() const override { return _available; }
    uint32_t getCode()       const override { return _code; }
    uint8_t  getProtocol()   const override { return _protocol; }

    // Called from the RMT ISR; do not call directly.
    bool onFrame(const void *edata);

private:
    static constexpr uint8_t RING_CAP = 8;

    struct Ring {
        volatile uint8_t  head  = 0;
        volatile uint8_t  tail  = 0;
        volatile uint8_t  proto[RING_CAP] = {};
        volatile uint8_t  nbits[RING_CAP] = {};
        volatile uint32_t code [RING_CAP] = {};
    };
    mutable Ring _ring;

    void pushEvent(uint32_t code, uint8_t proto, uint8_t nbits);
    void rearm();

    int   _gpio;
    bool  _active    = false;
    bool  _wallMode  = false;
    uint8_t _wallMarks = 0;
    mutable bool     _available = false;
    mutable uint32_t _code      = 0;
    mutable uint8_t  _protocol  = 0;

    NecDecoder  _nec;
    SonyDecoder _sony;

    // RMT handle stored as void* to keep esp-idf RMT header out of this header.
    void *_rx_chan = nullptr;   // rmt_channel_handle_t

    // Symbol buffer: rmt_symbol_word_t is 4 bytes; 64 slots covers NEC 32-bit frame.
    static constexpr size_t SYM_CAP = 64;
    uint32_t _sym_buf[SYM_CAP];

    static Esp32IRModule *s_instance;
};
