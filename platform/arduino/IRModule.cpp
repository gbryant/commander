#define RAW_BUFFER_LENGTH 80  // NEC needs 67; 80 gives headroom without wasting RAM
#include <Arduino.h>          // must precede IRremote.hpp for __FlashStringHelper
#include <IRremote.hpp>
#include "IRModule.h"
#include "core/Writer.h"
#include "modules/ir/IrEvent.h"
#include <Arduino_FreeRTOS.h>

// ---------------------------------------------------------------------------
// Roomba virtual wall / lighthouse IR protocol (observed on TSOP 38kHz rx)
//
// The virtual wall transmits repeating 3-burst packets:
//   3× (~550µs mark  +  ~7350µs space), then a gap > 8ms.
// IRremote captures this as rawlen=6: [0, mark, space, mark, space, mark]
// where values are in MICROS_PER_TICK (50µs) units.
//
//   ROOMBA_MARK_TICKS  = 11  (~550µs)
//   ROOMBA_SPACE_TICKS = 147 (~7350µs)
// ---------------------------------------------------------------------------

#ifdef COMMANDER_IR_WALL
#define ROOMBA_MARK_TICKS    11
#define ROOMBA_SPACE_TICKS  147
#define ROOMBA_TOL_PCT       35

static bool inRange(uint8_t val, uint8_t center) {
    uint8_t delta = (uint8_t)((uint16_t)center * ROOMBA_TOL_PCT / 100);
    return val >= center - delta && val <= center + delta;
}

static int16_t decodeRoomba(const IRRawbufType *raw, uint16_t rawlen) {
    if (rawlen < 6) return -1;
    for (uint8_t i = 0; i < 5; i++) {
        uint8_t expected = (i % 2 == 0) ? ROOMBA_MARK_TICKS : ROOMBA_SPACE_TICKS;
        if (!inRange(raw[1 + i], expected)) return -1;
    }
    return 0xA5;
}
#endif // COMMANDER_IR_WALL

void IRModule::tick() {
    if (!_active && !_wallMode) return;
    if (!IrReceiver.decode()) return;

#ifdef COMMANDER_IR_WALL
    if (_wallMode) {
        int16_t b = decodeRoomba(IrReceiver.irparams.rawbuf,
                                  IrReceiver.decodedIRData.rawlen);
        if (b >= 0) {
            if (_out) _out->writeln("wall 0xA5");
            else      Serial.print(F("\r\n[Roomba] Virtual Wall (0xA5)"));
        }
        IrReceiver.resume();
        return;
    }
#endif
    _code      = IrReceiver.decodedIRData.decodedRawData;
    _protocol  = (uint8_t)IrReceiver.decodedIRData.protocol;
    _available = true;
    if (_out) {
        char line[20];
        ir_format_event(line, _code, _protocol);
        _out->writeln(line);                 // one frame on the ir channel, unsolicited
    } else {
        Serial.print(F("\r\n"));
        IrReceiver.printIRResultShort(&Serial);
    }
    IrReceiver.resume();
}

void IRModule::registerCommands(CommandRegistry &reg) {
    reg.registerCommand(CMD("ir recv", "toggle IR receive mode (pin D5)", CMD_IR_RECV,
        [](const char *, Writer &out, void *ctx) {
            auto *self = static_cast<IRModule *>(ctx);
            self->_wallMode = false;
            self->_active   = !self->_active;
            if (self->_active) {
                if (!self->_started) {
                    IrReceiver.begin(self->_pin, DISABLE_LED_FEEDBACK);
                    self->_started = true;
                }
                out.writeln("listening... (ir recv to stop)");
            } else {
                out.writeln("stopped.");
            }
        }, this));

#ifdef COMMANDER_IR_WALL
    reg.registerCommand(CMD("ir wall", "detect Roomba virtual wall transmissions", CMD_IR_WALL,
        [](const char *, Writer &out, void *ctx) {
            auto *self = static_cast<IRModule *>(ctx);
            self->_active   = false;
            self->_wallMode = !self->_wallMode;
            if (self->_wallMode) {
                if (!self->_started) {
                    IrReceiver.begin(self->_pin, DISABLE_LED_FEEDBACK);
                    self->_started = true;
                }
                out.writeln("watching for Roomba wall signals... (ir wall to stop)");
            } else {
                out.writeln("stopped.");
            }
        }, this));
#endif // COMMANDER_IR_WALL
}
