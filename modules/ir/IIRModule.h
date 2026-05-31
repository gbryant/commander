#pragma once
#include <stdint.h>
#include "core/IModule.h"

// IR receive is platform-specific (Arduino: IRremote, Pico: PIO, ESP32: RMT).
// Implement this interface in platform/*/IRModule.h and register from main.
class IIRModule : public IModule {
public:
    virtual bool     dataAvailable() const = 0;
    virtual uint32_t getCode()       const = 0;
    virtual uint8_t  getProtocol()   const = 0;
};
