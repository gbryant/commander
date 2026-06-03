#pragma once
#include "core/IModule.h"
#include "core/CommandRegistry.h"
#include "modules/locomotion/LocoProtocol.h"
#include "hal/hal.h"
#include <stdlib.h>   // strtol
#include <string.h>   // strncmp, strlen

// Master side of the locomotion link. Runs on the main controller (Pico) and
// drives a remote mobile base over I2C — today an Arduino R4 acting as a Roomba
// bridge (see LocomotionBridge on the R4). Platform-independent: every command
// is a single hal_i2c_* transfer to the bridge address, with the command byte
// from i2c_ids.h as the I2C "register" and LocoProtocol.h as the payload format.
//
//   drive forward|back|left|right [speed_mmps]   (continuous; 'stop' to halt)
//   drive <vel> <radius>                          (raw OI velocity/radius)
//   stop
//   loco            (usage)
//   loco sensors    (read + print the base's sensor snapshot)
//
// The bus must already be up (hal_i2c_init) — the generated commander_modules.h
// brings it up for this module, the same way it does for compass.
class LocomotionModule : public IModule {
public:
    explicit LocomotionModule(uint8_t bridgeAddr) : _addr(bridgeAddr) {}

    const char *name() const override { return "locomotion"; }
    void        init() override {}
    void        registerCommands(CommandRegistry &reg) override;

private:
    uint8_t _addr;

    static void driveCmd(const char *args, Writer &out, void *ctx);
    static void stopCmd(const char *args, Writer &out, void *ctx);
    static void locoCmd(const char *args, Writer &out, void *ctx);
    static void bridgeCmd(const char *args, Writer &out, void *ctx);

    bool sendDrive(int16_t vel, int16_t radius);
    bool sendStop();
    void readSensors(Writer &out);
    void remoteExec(const char *cmd, Writer &out);   // run a command on the bridge board

    static void usage(Writer &out);

    // No-printf helpers, matching the codebase's manual number formatting.
    static const char *fmtInt(int32_t v, char *buf);
    static const char *skipSpaces(const char *p) { while (*p == ' ') ++p; return p; }
    static const char *nextTok(const char *p) { while (*p && *p != ' ') ++p; return skipSpaces(p); }
    static bool tokIs(const char *p, const char *t) {
        size_t n = strlen(t);
        return strncmp(p, t, n) == 0 && (p[n] == '\0' || p[n] == ' ');
    }
};

inline const char *LocomotionModule::fmtInt(int32_t v, char *buf) {
    char tmp[12]; int t = 0;
    bool neg = v < 0;
    uint32_t u = neg ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
    if (u == 0) tmp[t++] = '0';
    while (u) { tmp[t++] = (char)('0' + u % 10); u /= 10; }
    int i = 0;
    if (neg) buf[i++] = '-';
    while (t) buf[i++] = tmp[--t];
    buf[i] = '\0';
    return buf;
}

inline void LocomotionModule::usage(Writer &out) {
    out.writeln("drive forward|back|left|right [speed_mmps]   (continuous - 'stop' to halt)");
    out.writeln("drive <vel> <radius>                          (raw OI velocity/radius)");
    out.writeln("stop                                          (halt the base)");
    out.writeln("loco sensors                                  (read base sensor snapshot)");
}

inline bool LocomotionModule::sendDrive(int16_t vel, int16_t radius) {
    uint8_t payload[LOCO_DRIVE_LEN];
    loco_pack_drive(vel, radius, payload);
    return hal_i2c_write(_addr, CMD_LOCO_DRIVE, payload, LOCO_DRIVE_LEN);
}

inline bool LocomotionModule::sendStop() {
    return hal_i2c_write(_addr, CMD_LOCO_STOP, nullptr, 0);
}

inline void LocomotionModule::readSensors(Writer &out) {
    uint8_t buf[LOCO_SENSORS_LEN];
    if (!hal_i2c_read(_addr, CMD_LOCO_SENSORS, buf, LOCO_SENSORS_LEN)) {
        out.writeln("sensor read failed (bridge not responding)");
        return;
    }
    LocoSensors s;
    loco_unpack_sensors(buf, &s);
    char b[12];
    out.write("bumps L/R: ");
    out.write((s.flags1 & LOCO_F1_BUMP_LEFT) ? "1" : "0"); out.write("/");
    out.writeln((s.flags1 & LOCO_F1_BUMP_RIGHT) ? "1" : "0");
    out.write("cliffs L/FL/FR/R: ");
    out.write((s.flags2 & LOCO_F2_CLIFF_LEFT) ? "1" : "0"); out.write("/");
    out.write((s.flags2 & LOCO_F2_CLIFF_FL) ? "1" : "0"); out.write("/");
    out.write((s.flags2 & LOCO_F2_CLIFF_FR) ? "1" : "0"); out.write("/");
    out.writeln((s.flags2 & LOCO_F2_CLIFF_RIGHT) ? "1" : "0");
    out.write("wall: "); out.writeln((s.flags1 & LOCO_F1_WALL) ? "1" : "0");
    out.write("dist/angle: "); out.write(fmtInt(s.distance_mm, b)); out.write("mm / ");
    out.write(fmtInt(s.angle_deg, b)); out.writeln("deg");
    out.write("battery: "); out.write(fmtInt(s.battery_pct, b)); out.write("%  ");
    out.write(fmtInt(s.voltage_mv, b)); out.write(" mV  ");
    out.write(fmtInt(s.current_ma, b)); out.writeln(" mA");
}

inline void LocomotionModule::driveCmd(const char *args, Writer &out, void *ctx) {
    LocomotionModule *self = static_cast<LocomotionModule *>(ctx);
    const char *p = skipSpaces(args);
    if (*p == '\0' || tokIs(p, "help")) { usage(out); return; }

    int dir = 0;  // 1=fwd, -1=back, 2=left, 3=right, 0=raw
    if      (tokIs(p, "forward") || tokIs(p, "fwd"))                       dir = 1;
    else if (tokIs(p, "back") || tokIs(p, "backward") || tokIs(p, "rev")) dir = -1;
    else if (tokIs(p, "left"))                                            dir = 2;
    else if (tokIs(p, "right"))                                           dir = 3;

    if (dir == 0) {
        // raw: drive <vel> <radius>
        char *end;
        long vel = strtol(p, &end, 10);
        if (end == p) { usage(out); return; }
        long rad = strtol(end, &end, 10);
        out.writeln(self->sendDrive((int16_t)vel, (int16_t)rad) ? "ok: drive (raw)"
                                                                : "i2c write failed");
        return;
    }

    const char *q = nextTok(p);   // token after the direction word
    char *e1;
    long speed = strtol(q, &e1, 10);
    if (e1 == q) speed = 200;     // default speed
    if (speed < 0)   speed = 0;
    if (speed > 500) speed = 500;

    int16_t v = (int16_t)speed, radius = LOCO_RADIUS_STRAIGHT;
    switch (dir) {
        case 1:  v =  (int16_t)speed; radius = LOCO_RADIUS_STRAIGHT; break;
        case -1: v = -(int16_t)speed; radius = LOCO_RADIUS_STRAIGHT; break;
        case 2:  v =  (int16_t)speed; radius = LOCO_RADIUS_CCW; break;  // spin left
        case 3:  v =  (int16_t)speed; radius = LOCO_RADIUS_CW;  break;  // spin right
    }
    out.writeln(self->sendDrive(v, radius)
                    ? "ok: drive (continuous - 'stop' to halt)"
                    : "i2c write failed");
}

inline void LocomotionModule::stopCmd(const char *args, Writer &out, void *ctx) {
    (void)args;
    LocomotionModule *self = static_cast<LocomotionModule *>(ctx);
    out.writeln(self->sendStop() ? "ok: stop" : "i2c write failed");
}

inline void LocomotionModule::locoCmd(const char *args, Writer &out, void *ctx) {
    LocomotionModule *self = static_cast<LocomotionModule *>(ctx);
    const char *p = skipSpaces(args);
    if (tokIs(p, "sensors")) { self->readSensors(out); return; }
    usage(out);
}

// Run a command line on the bridge board's shell over I2C and print the output —
// a remote console for when the bridge's own transport (e.g. R4 WiFi) is down.
inline void LocomotionModule::remoteExec(const char *cmd, Writer &out) {
    size_t len = strlen(cmd);
    if (len > LOCO_CONSOLE_CMD_MAX - 1) len = LOCO_CONSOLE_CMD_MAX - 1;
    if (!hal_i2c_write(_addr, CMD_CONSOLE_EXEC, (const uint8_t *)cmd, len)) {
        out.writeln("bridge not responding (i2c write failed)");
        return;
    }
    uint8_t chunk[1 + LOCO_CONSOLE_CHUNK];
    for (int tries = 0; tries < 200; tries++) {     // ~busy-poll; I2C paces it
        if (!hal_i2c_read(_addr, CMD_CONSOLE_READ, chunk, sizeof(chunk))) {
            out.writeln("bridge read failed");
            return;
        }
        uint8_t ctrl = chunk[0];
        if (ctrl == LOCO_CONSOLE_BUSY) continue;    // not dispatched yet
        if (ctrl == 0) return;                      // output complete
        char tmp[LOCO_CONSOLE_CHUNK + 1];
        uint8_t i = 0;
        for (; i < ctrl && i < LOCO_CONSOLE_CHUNK; i++) tmp[i] = (char)chunk[1 + i];
        tmp[i] = '\0';
        out.write(tmp);
    }
    out.writeln("(bridge: timed out waiting for output)");
}

inline void LocomotionModule::bridgeCmd(const char *args, Writer &out, void *ctx) {
    LocomotionModule *self = static_cast<LocomotionModule *>(ctx);
    const char *p = skipSpaces(args);
    if (*p == '\0') {
        out.writeln("bridge <command>   run a command on the bridge board's shell");
        out.writeln("bridge reset       hard-reset the bridge board");
        return;
    }
    if (tokIs(p, "reset")) {
        out.writeln(hal_i2c_write(self->_addr, CMD_RESET, nullptr, 0)
                        ? "ok: reset sent to bridge" : "i2c write failed");
        return;
    }
    self->remoteExec(p, out);
}

inline void LocomotionModule::registerCommands(CommandRegistry &reg) {
    reg.registerCommand(CMD("drive",  "Drive the base - 'drive' for usage", I2C_NONE, driveCmd,  this));
    reg.registerCommand(CMD("stop",   "Halt the base",                      I2C_NONE, stopCmd,   this));
    reg.registerCommand(CMD("loco",   "Locomotion status - 'loco sensors'", I2C_NONE, locoCmd,   this));
    reg.registerCommand(CMD("bridge", "Remote console to the bridge board - 'bridge'", I2C_NONE, bridgeCmd, this));
}
