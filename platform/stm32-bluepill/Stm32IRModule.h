#pragma once
#include "modules/ir/IIRModule.h"
#include "modules/ir/NecDecoder.h"
#include "modules/ir/SonyDecoder.h"
#include "core/CommandRegistry.h"
#include "core/Writer.h"
#include <stdint.h>

// IR receive for STM32F103 via EXTI edge interrupt + DWT cycle counter.
// The ISR reads the pin level after each edge, computes the interval (DWT CYCCNT /
// 72 = µs), and feeds NecDecoder/SonyDecoder. Results are pushed into a SPSC ring
// and drained in tick().
//
// The EXTI handlers (EXTI0–EXTI4, EXTI9_5, EXTI15_10) are defined in
// Stm32IRModule.cpp; only the handler matching the configured pin bit fires.
// The IR module owns whichever EXTI line it is configured on — do not define
// competing handlers in the same project.
//
// Default pin: 0x10 (PB0 — EXTI0, dedicated vector).
// HAL pin encoding: (port << 4) | bit, e.g. PB3 = 0x13, PA4 = 0x04.
class Stm32IRModule : public IIRModule {
public:
    static constexpr uint8_t PROTO_NEC  = 1;
    static constexpr uint8_t PROTO_SONY = 2;
    static constexpr uint8_t PROTO_WALL = 3;

    explicit Stm32IRModule(uint8_t pin = 0x10) : _pin(pin) {}

    const char *name() const override { return "ir"; }
    void        init() override;
    void        registerCommands(CommandRegistry &reg) override;
    void        tick() override;

    bool     dataAvailable() const override { return _available; }
    uint32_t getCode()       const override { return _code; }
    uint8_t  getProtocol()   const override { return _protocol; }

    // Called from the EXTI ISR; do not call directly.
    void onEdge();

    volatile bool _active   = false;
    volatile bool _wallMode = false;

    static Stm32IRModule *s_instance;

private:
    bool start();

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

    uint8_t  _pin;
    bool     _started  = false;
    uint32_t _last_cyc = 0;
    uint8_t  _wallMarks = 0;

    mutable bool     _available = false;
    mutable uint32_t _code      = 0;
    mutable uint8_t  _protocol  = 0;

    NecDecoder  _nec;
    SonyDecoder _sony;
};
