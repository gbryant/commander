#pragma once
#define IR_RECEIVE_PIN 5  // Grove D4/D5 connector — D5 is INT0-capable

#include "modules/ir/IIRModule.h"
#include "core/CommandRegistry.h"

class IRModule : public IIRModule {
public:
    const char *name()  const override { return "ir"; }
    void        init()        override {}
    void        registerCommands(CommandRegistry &reg) override;
    void        tick()                  override;

    bool     dataAvailable() const override { return _available; }
    uint32_t getCode()       const override { return _code; }
    uint8_t  getProtocol()   const override { return _protocol; }

    volatile bool _active   = false;
    volatile bool _wallMode = false;
    bool          _started  = false;
    volatile bool _available = false;
    uint32_t      _code      = 0;
    uint8_t       _protocol  = 0;

private:
    static void tickTask(void *arg);
};
