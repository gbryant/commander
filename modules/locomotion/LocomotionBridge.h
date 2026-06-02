#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <string.h>
#include "core/IModule.h"
#include "core/CommandRegistry.h"
#include "modules/roomba/Roomba.h"
#include "modules/locomotion/LocoProtocol.h"

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
    LocomotionBridge(Roomba &roomba, uint8_t addr) : _roomba(roomba), _addr(addr) {}

    const char *name() const override { return "loco-bridge"; }

    void init() override {
        _self = this;
        Wire.begin(_addr);              // join the bus as a slave
        Wire.onReceive(onReceiveThunk); // [reg][payload] from the master
        Wire.onRequest(onRequestThunk); // master reads the sensor snapshot
    }

    void registerCommands(CommandRegistry &) override {}  // no shell commands of its own

    // Pumped by the UART task. Applies any latched drive/stop command and keeps
    // the sensor cache fresh so onRequest can answer without blocking I/O.
    void tick() override {
        if (_pendingDrive) {
            uint8_t buf[LOCO_DRIVE_LEN];
            noInterrupts();
            for (int i = 0; i < LOCO_DRIVE_LEN; i++) buf[i] = _driveBuf[i];
            _pendingDrive = false;
            interrupts();
            int16_t vel; int16_t radius;
            loco_unpack_drive(buf, &vel, &radius);
            _roomba.drive(vel, radius);
        }
        if (_pendingStop) {
            _pendingStop = false;
            _roomba.stop();
        }
        uint32_t now = millis();
        if (now - _lastSensorMs >= kSensorIntervalMs) {
            _lastSensorMs = now;
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

private:
    static constexpr uint32_t kSensorIntervalMs = 200;

    Roomba &_roomba;
    uint8_t _addr;

    // ISR <-> task shared state. Written in the Wire ISR, read/cleared in tick().
    volatile bool    _pendingDrive = false;
    volatile bool    _pendingStop  = false;
    volatile uint8_t _driveBuf[LOCO_DRIVE_LEN]      = {0};
    volatile uint8_t _sensorCache[LOCO_SENSORS_LEN] = {0};
    volatile uint8_t _lastReg = I2C_NONE;
    uint32_t _lastSensorMs = 0;

    // Wire callbacks are plain C function pointers, so route them through a
    // singleton. Inline static (C++17) — header is included in one TU on R4.
    static inline LocomotionBridge *_self = nullptr;
    static void onReceiveThunk(int n) { if (_self) _self->onReceive(n); }
    static void onRequestThunk()      { if (_self) _self->onRequest(); }

    void onReceive(int n) {
        if (n <= 0) return;
        uint8_t reg = (uint8_t)Wire.read();   // first byte = command / register
        _lastReg = reg;
        if (reg == CMD_LOCO_DRIVE) {
            for (int i = 0; i < LOCO_DRIVE_LEN && Wire.available(); i++)
                _driveBuf[i] = (uint8_t)Wire.read();
            _pendingDrive = true;
        } else if (reg == CMD_LOCO_STOP) {
            _pendingStop = true;
        }
        // CMD_LOCO_SENSORS is set-register-only here; the data goes out via the
        // repeated-start read handled by onRequest().
        while (Wire.available()) Wire.read();  // drain any trailing bytes
    }

    void onRequest() {
        // Answer instantly from the cache — never block in the ISR.
        if (_lastReg == CMD_LOCO_SENSORS) {
            uint8_t buf[LOCO_SENSORS_LEN];
            for (int i = 0; i < LOCO_SENSORS_LEN; i++) buf[i] = _sensorCache[i];
            Wire.write(buf, LOCO_SENSORS_LEN);
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
