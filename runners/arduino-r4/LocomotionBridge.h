#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <string.h>
#include "core/IModule.h"
#include "core/CommandRegistry.h"
#include "modules/roomba/Roomba.h"
#include "modules/locomotion/LocoProtocol.h"

#ifndef LOCO_CONSOLE_CMD_MAX
#define LOCO_CONSOLE_CMD_MAX 32     // max remote command-line length (fits Wire RX)
#endif
#ifndef LOCO_CONSOLE_OUT_MAX
#define LOCO_CONSOLE_OUT_MAX 128    // captured output buffer (R4 RAM is tight; long
#endif                             // output truncates — override -D to enlarge)
#ifndef LOCO_IDLE_MS
#define LOCO_IDLE_MS       120000u  // idle this long → park the base (OI Stop, charging/sleep allowed)
#endif

// R4 side of the locomotion link: an I2C slave at LOCO_BRIDGE_ADDR that presents
// the generic mobile-base command set (i2c_ids.h CMD_LOCO_*) and forwards it to a
// shared Roomba OI driver. The Pico master (LocomotionModule) drives this.
//
// ⚠️ Wire's onReceive/onRequest run in ISR context, so they must be fast and must
// never block. Roomba.drive()/stop()/readAllSensors() all block (Serial1 + delay),
// so the callbacks only LATCH the inbound command and answer onRequest from a
// PRE-CACHED snapshot. The real (blocking) Roomba I/O happens in tick(), which the
// UART task pumps via uart.addTicker() — the same pattern the IR recv module uses.
//
// One driver, one Serial1: the bridge shares the Roomba instance that the roomba
// module's `oi` command uses (see RoombaModule::driver()), so direct debugging and
// the I2C bridge never fight over the serial line.
class LocomotionBridge : public IModule {
public:
    // `wire` selects the I2C port. On the Uno R4 WiFi:
    //   Wire  — A4/A5 header pins (IIC1), 5V logic — needs a level shifter to a
    //           3.3V master like the Pico 2 W.
    //   Wire1 — the Qwiic/STEMMA QT connector (IIC0), 3.3V — wire a 3.3V master
    //           straight in. Both map to a hardware IIC peripheral, so both
    //           support I2C slave mode (an SCI-backed Wire would not).
    LocomotionBridge(Roomba &roomba, uint8_t addr, TwoWire &wire = Wire)
        : _roomba(roomba), _addr(addr), _wire(wire) {}

    const char *name() const override { return "loco-bridge"; }

    void init() override {
        _self = this;
        _wire.begin(_addr);              // join the bus as a slave
        _wire.onReceive(onReceiveThunk); // [reg][payload] from the master
        _wire.onRequest(onRequestThunk); // master reads the sensor snapshot
    }

    // Keep the registry so the remote console can dispatch arbitrary commands.
    void registerCommands(CommandRegistry &reg) override { _reg = &reg; }

    // Pumped by the UART task. Applies any latched drive/stop command, runs any
    // latched remote-console command, and keeps the sensor cache fresh — all the
    // blocking work the ISR callbacks defer.
    void tick() override {
        if (_resetPending) { _resetPending = false; delay(5); NVIC_SystemReset(); }
        if (_consoleState == 1 && _reg) {       // dispatch a latched command line
            char cmd[LOCO_CONSOLE_CMD_MAX];
            noInterrupts();
            uint8_t n = _cmdLen;
            for (uint8_t i = 0; i < n; i++) cmd[i] = (char)_cmdBuf[i];
            interrupts();
            cmd[n] = '\0';
            CaptureWriter cw(_outBuf, LOCO_CONSOLE_OUT_MAX);
            _reg->dispatch(cmd, cw);
            noInterrupts();
            _outLen = cw.len; _outPos = 0; _consoleState = 2;  // output ready
            interrupts();
        }
        if (_pendingDrive) {
            uint8_t buf[LOCO_DRIVE_LEN];
            noInterrupts();
            for (int i = 0; i < LOCO_DRIVE_LEN; i++) buf[i] = _driveBuf[i];
            _pendingDrive = false;
            interrupts();
            int16_t vel; int16_t radius;
            loco_unpack_drive(buf, &vel, &radius);
            _roomba.drive(vel, radius);          // re-inits (wakes) the base if parked
            _lastDriveMs = millis();
            _idleParked = false;
        }
        if (_pendingStop) {
            _pendingStop = false;
            _roomba.stop();
            _lastDriveMs = millis();
            _idleParked = false;
        }

        // Lazy sensor read: only when the master asked (a CMD_LOCO_SENSORS write
        // sets _sensorRefresh) — never on a free-running timer. The old 200 ms poll
        // was a blocking Serial1 read that stalled the drive stream (stutter) and
        // kept the base awake forever (battery drain). Don't wake a parked base
        // just to read sensors — serve the last snapshot instead.
        if (_sensorRefresh) {
            _sensorRefresh = false;
            if (!_idleParked) {
                RoombaSensors rs;
                if (_roomba.readAllSensors(rs)) {
                    LocoSensors ls;
                    fromRoomba(rs, ls);
                    uint8_t buf[LOCO_SENSORS_LEN];
                    loco_pack_sensors(&ls, buf);
                    noInterrupts();
                    for (int i = 0; i < LOCO_SENSORS_LEN; i++) _sensorCache[i] = buf[i];
                    interrupts();
                }
            }
        }

        // Idle power: after no drive for LOCO_IDLE_MS, park the base (OI Stop →
        // motors off, charging/sleep allowed). The next drive re-inits it
        // (drive() → start(), which pulses BRC to wake if a BRC line is wired; with
        // no BRC, a deep-slept base needs a manual button press to wake).
        uint32_t now = millis();
        if (!_idleParked && _lastDriveMs != 0 && (now - _lastDriveMs) >= LOCO_IDLE_MS) {
            _roomba.disconnect();
            _idleParked = true;
        }
    }

private:
    Roomba &_roomba;
    uint8_t _addr;
    TwoWire &_wire;

    // ISR <-> task shared state. Written in the Wire ISR, read/cleared in tick().
    volatile bool    _pendingDrive = false;
    volatile bool    _pendingStop  = false;
    volatile uint8_t _driveBuf[LOCO_DRIVE_LEN]      = {0};
    volatile uint8_t _sensorCache[LOCO_SENSORS_LEN] = {0};
    volatile uint8_t _lastReg = I2C_NONE;
    volatile bool    _sensorRefresh = false;  // master asked for a sensor snapshot
    uint32_t _lastDriveMs = 0;   // last drive/stop applied — drives the idle policy
    bool     _idleParked  = false;  // base parked (OI Stop) for idle power save

    // Remote console (CMD_CONSOLE_EXEC/READ) + CMD_RESET. Lets a master drive this
    // board's shell over I2C when its own transport (R4 WiFi/telnet) is unreachable.
    CommandRegistry *_reg = nullptr;
    volatile uint8_t  _consoleState = 0;   // 0 idle, 1 cmd latched, 2 output ready
    volatile bool     _resetPending = false;
    volatile uint8_t  _cmdBuf[LOCO_CONSOLE_CMD_MAX] = {0};
    volatile uint8_t  _cmdLen = 0;
    uint8_t           _outBuf[LOCO_CONSOLE_OUT_MAX];   // filled in tick(), read in onRequest()
    volatile uint16_t _outLen = 0;
    volatile uint16_t _outPos = 0;

    // Captures a dispatched command's output into _outBuf for the master to read.
    struct CaptureWriter : public Writer {
        uint8_t *buf; uint16_t cap; uint16_t len = 0;
        CaptureWriter(uint8_t *b, uint16_t c) : buf(b), cap(c) {}
        void write(const char *s) override { while (*s && len < cap) buf[len++] = (uint8_t)*s++; }
        void writeln(const char *s = "") override { write(s); write("\r\n"); }
    };

    // Wire callbacks are plain C function pointers, so route them through a
    // singleton. Inline static (C++17) — header is included in one TU on R4.
    static inline LocomotionBridge *_self = nullptr;
    static void onReceiveThunk(int n) { if (_self) _self->onReceive(n); }
    static void onRequestThunk()      { if (_self) _self->onRequest(); }

    void onReceive(int n) {
        if (n <= 0) return;
        uint8_t reg = (uint8_t)_wire.read();   // first byte = command / register
        _lastReg = reg;
        if (reg == CMD_LOCO_DRIVE) {
            for (int i = 0; i < LOCO_DRIVE_LEN && _wire.available(); i++)
                _driveBuf[i] = (uint8_t)_wire.read();
            _pendingDrive = true;
        } else if (reg == CMD_LOCO_STOP) {
            _pendingStop = true;
        } else if (reg == CMD_CONSOLE_EXEC) {
            uint8_t n = 0;                       // latch the command line for tick()
            while (_wire.available() && n < LOCO_CONSOLE_CMD_MAX - 1)
                _cmdBuf[n++] = (uint8_t)_wire.read();
            _cmdLen = n;
            _consoleState = 1;
        } else if (reg == CMD_RESET) {
            _resetPending = true;
        } else if (reg == CMD_LOCO_SENSORS) {
            _sensorRefresh = true;               // tick() does the (blocking) read
        }
        // The CMD_LOCO_SENSORS payload goes out via the repeated-start read handled
        // by onRequest() (served from the cache tick() just refreshed); same for
        // CMD_CONSOLE_READ.
        while (_wire.available()) _wire.read();  // drain any trailing bytes
    }

    void onRequest() {
        // Answer instantly from a cache/buffer — never block in the ISR.
        if (_lastReg == CMD_LOCO_SENSORS) {
            uint8_t buf[LOCO_SENSORS_LEN];
            for (int i = 0; i < LOCO_SENSORS_LEN; i++) buf[i] = _sensorCache[i];
            _wire.write(buf, LOCO_SENSORS_LEN);
        } else if (_lastReg == CMD_CONSOLE_READ) {
            uint8_t chunk[1 + LOCO_CONSOLE_CHUNK] = {0};
            if (_consoleState == 1) {
                chunk[0] = LOCO_CONSOLE_BUSY;     // not dispatched yet — master retries
            } else {
                uint8_t n = 0;
                while (n < LOCO_CONSOLE_CHUNK && _outPos < _outLen)
                    chunk[1 + n++] = _outBuf[_outPos++];
                chunk[0] = n;                     // 0 = output complete
                if (_outPos >= _outLen) _consoleState = 0;
            }
            _wire.write(chunk, sizeof(chunk));
        }
    }

    static void fromRoomba(const RoombaSensors &r, LocoSensors &l) {
        l.flags1 = (uint8_t)((r.bumpLeft        ? LOCO_F1_BUMP_LEFT  : 0) |
                             (r.bumpRight        ? LOCO_F1_BUMP_RIGHT : 0) |
                             (r.wheelDropLeft    ? LOCO_F1_DROP_LEFT  : 0) |
                             (r.wheelDropRight   ? LOCO_F1_DROP_RIGHT : 0) |
                             (r.wheelDropCaster  ? LOCO_F1_DROP_CASTER: 0) |
                             (r.wall             ? LOCO_F1_WALL       : 0) |
                             (r.virtualWall      ? LOCO_F1_VWALL      : 0));
        l.flags2 = (uint8_t)((r.cliffLeft        ? LOCO_F2_CLIFF_LEFT : 0) |
                             (r.cliffFrontLeft   ? LOCO_F2_CLIFF_FL   : 0) |
                             (r.cliffFrontRight  ? LOCO_F2_CLIFF_FR   : 0) |
                             (r.cliffRight       ? LOCO_F2_CLIFF_RIGHT: 0));
        l.distance_mm    = r.distance;
        l.angle_deg      = r.angle;
        l.battery_pct    = r.batteryCapacity
                             ? (uint8_t)(((uint32_t)r.batteryCharge * 100) / r.batteryCapacity)
                             : 0;
        l.charging_state = r.chargingState;
        l.voltage_mv     = r.voltage;
        l.current_ma     = r.current;
    }
};
