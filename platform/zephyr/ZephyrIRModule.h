#pragma once
#include "modules/ir/IIRModule.h"
#include "modules/ir/NecDecoder.h"
#include "modules/ir/SonyDecoder.h"
#include "core/CommandRegistry.h"
#include "core/Writer.h"
#include <zephyr/drivers/gpio.h>

// IR receive for the Zephyr HAL (first target: Arduino Uno Q / STM32U585 M33). There is no
// IRremote on Zephyr, so this is a from-scratch NEC receiver: a GPIO edge interrupt on the
// IR pin timestamps marks/spaces with the CPU cycle counter (wrap-safe µs deltas) and feeds
// the portable NecDecoder. Decoded codes are latched in a tiny single-producer/single-
// consumer ring in the ISR and drained in tick() — so all UART/publish work runs in thread
// context, never the ISR. setOutput() routes each press to a channel publisher exactly like
// the Arduino IRModule, so `ir recv` events frame onto the `ir` channel.
//
// The pin comes from devicetree: add to the app overlay
//     / { zephyr,user { ir-gpios = <&arduino_header 11 GPIO_ACTIVE_HIGH>; }; };   // D5
// and CONFIG_GPIO=y. Needs a µs-resolution cycle counter (Cortex-M systick at the CPU clock
// gives this on the U585 — verify on other boards).
class ZephyrIRModule : public IIRModule {
public:
    const char *name() const override { return "ir"; }
    void        init()       override {}
    void        registerCommands(CommandRegistry &reg) override;
    void        tick()       override;

    bool     dataAvailable() const override { return _code_valid; }
    uint32_t getCode()       const override { return _last; }
    uint8_t  getProtocol()   const override { return kProtocolNec; }

    void setOutput(Writer *w) { _out = w; }

    // ISR hook — public so the C gpio_callback trampoline can reach it. Do not call directly.
    void onEdge();

    volatile bool _active = false;          // toggled by `ir recv`; gates the ISR + publish

private:
    bool start();                           // configure the pin + edge interrupt (once)

    static constexpr uint8_t kProtocolNec  = 3;
    static constexpr uint8_t kProtocolSony = 4;
    static constexpr uint8_t RING = 8;      // power-friendly small SPSC ring of decoded events

    struct Ev { uint32_t code; uint8_t proto; uint8_t bits; };  // a decoded IR event
    void push(uint32_t code, uint8_t proto, uint8_t bits);      // latch into the SPSC ring

    NecDecoder  _dec;                       // NEC + Sony run in parallel on the same edges;
    SonyDecoder _son;                       // whichever recognizes the frame publishes.
    Writer    *_out      = nullptr;
    bool       _started  = false;
    uint32_t   _last_cyc = 0;

    volatile Ev      _ring[RING] = {};
    volatile uint8_t _head = 0;             // ISR writes _head
    volatile uint8_t _tail = 0;             // tick() writes _tail
    uint32_t   _last       = 0;
    bool       _code_valid = false;
};
