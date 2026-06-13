#pragma once
#define IR_RECEIVE_PIN 5  // default: Grove D4/D5 connector

#include "modules/ir/IIRModule.h"
#include "core/CommandRegistry.h"
#include "core/Writer.h"

class IRModule : public IIRModule {
public:
    explicit IRModule(uint8_t pin = IR_RECEIVE_PIN) : _pin(pin) {}

    const char *name()  const override { return "ir"; }
    void        init()        override {}
    void        registerCommands(CommandRegistry &reg) override;
    void        tick()                  override;

    // Route async recv events to an injected sink instead of the hardcoded Serial console
    // — on a channel-bus build the app sets this to a ChannelPublisher (e.g. ct.publisher(1))
    // in commander_on_channels_ready, so each IR press frames onto the `ir` channel. Null
    // (default) keeps the legacy console behavior, so plain UART builds are unchanged.
    void setOutput(Writer *w) { _out = w; }

    bool     dataAvailable() const override { return _available; }
    uint32_t getCode()       const override { return _code; }
    uint8_t  getProtocol()   const override { return _protocol; }

    volatile bool _active   = false;
    volatile bool _wallMode = false;
    bool          _started  = false;
    volatile bool _available = false;
    uint32_t      _code      = 0;
    uint8_t       _protocol  = 0;
    uint8_t       _pin;
    Writer       *_out       = nullptr;   // async event sink (channel publisher); null = Serial

private:
    static void tickTask(void *arg);
};
