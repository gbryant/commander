#pragma once
#include "core/IModule.h"
#include "core/CommandRegistry.h"
#include "modules/roomba/Roomba.h"
#include <stdlib.h>   // strtol
#include <string.h>   // strncmp, strlen

// Exposes the Roomba OI driver as a single shell command, "oi":
//   oi start | safe | full | passive | wake | ping (is the base awake?)
//   oi drive forward|back|left|right [speed_mmps] [ms]   (omit ms = continuous)
//   oi drive <vel> <radius>                              (raw OI drive)
//   oi stop | clean | spot | max | dock | power | reset | disconnect
//   oi sensors | battery
//
// Platform-independent: the board supplies a RoombaPort (see Roomba.h). One
// registry slot (text-only, I2C_NONE) until the I2C bridge protocol is defined.
class RoombaModule : public IModule {
public:
    explicit RoombaModule(RoombaPort &port) : _port(port) {}

    const char *name() const override { return "roomba"; }
    void        init() override { _roomba.begin(_port); }
    void        registerCommands(CommandRegistry &reg) override;

    // Expose the OI driver so another module on the same board (e.g. the R4
    // locomotion bridge) can share this one instance — one driver, one Serial1.
    Roomba &driver() { return _roomba; }

private:
    RoombaPort &_port;
    Roomba      _roomba;

    static void oiCmd(const char *args, Writer &out, void *ctx);
    static void driveCmd(RoombaModule *self, const char *p, Writer &out);
    static void dumpSensors(Roomba &r, Writer &out);
    static void dumpBattery(Roomba &r, Writer &out);
    static void usage(Writer &out);

    // No-printf helpers (matches the codebase's manual number formatting).
    static const char *fmtInt(int32_t v, char *buf);
    static const char *skipSpaces(const char *p) { while (*p == ' ') ++p; return p; }
    static const char *nextTok(const char *p) { while (*p && *p != ' ') ++p; return skipSpaces(p); }
    static bool tokIs(const char *p, const char *t) {
        size_t n = strlen(t);
        return strncmp(p, t, n) == 0 && (p[n] == '\0' || p[n] == ' ');
    }
};

inline const char *RoombaModule::fmtInt(int32_t v, char *buf) {
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

inline void RoombaModule::usage(Writer &out) {
    out.writeln("oi: start|safe|full|passive|wake|ping|stop|clean|spot|max|dock|power|reset|disconnect");
    out.writeln("    drive forward|back|left|right [speed_mmps] [ms]   (omit ms = continuous)");
    out.writeln("    drive <vel> <radius>   |   sensors | battery");
}

inline void RoombaModule::dumpBattery(Roomba &r, Writer &out) {
    RoombaSensors s;
    if (!r.readAllSensors(s)) { out.writeln("sensor read failed"); return; }
    char b[12];
    out.write("battery: "); out.write(fmtInt(r.getBatteryPercent(s), b)); out.write("%  ");
    out.write(fmtInt(s.voltage, b)); out.write(" mV  ");
    out.write(fmtInt(s.current, b)); out.write(" mA  ");
    out.write(fmtInt(s.temperature, b)); out.write("C  ");
    out.writeln(r.getChargingStateString(s.chargingState));
}

inline void RoombaModule::dumpSensors(Roomba &r, Writer &out) {
    RoombaSensors s;
    if (!r.readAllSensors(s)) { out.writeln("sensor read failed"); return; }
    char b[12];
    out.write("bumps L/R: "); out.write(s.bumpLeft ? "1" : "0"); out.write("/"); out.writeln(s.bumpRight ? "1" : "0");
    out.write("cliffs L/FL/FR/R: ");
    out.write(s.cliffLeft ? "1" : "0"); out.write("/");
    out.write(s.cliffFrontLeft ? "1" : "0"); out.write("/");
    out.write(s.cliffFrontRight ? "1" : "0"); out.write("/");
    out.writeln(s.cliffRight ? "1" : "0");
    out.write("wall: "); out.writeln(s.wall ? "1" : "0");
    out.write("dist/angle: "); out.write(fmtInt(s.distance, b)); out.write("mm / ");
    out.write(fmtInt(s.angle, b)); out.writeln("deg");
    dumpBattery(r, out);
}

inline void RoombaModule::driveCmd(RoombaModule *self, const char *p, Writer &out) {
    Roomba &r = self->_roomba;
    p = skipSpaces(p);
    const int16_t STRAIGHT = (int16_t)0x8000;

    int dir = 0;  // 1=fwd, -1=back, 2=left, 3=right, 0=raw
    if      (tokIs(p, "forward") || tokIs(p, "fwd"))                       dir = 1;
    else if (tokIs(p, "back") || tokIs(p, "backward") || tokIs(p, "rev")) dir = -1;
    else if (tokIs(p, "left"))                                            dir = 2;
    else if (tokIs(p, "right"))                                           dir = 3;

    if (dir == 0) {
        // raw: oi drive <vel> <radius>
        char *end;
        long vel = strtol(p, &end, 10);
        if (end == p) { usage(out); return; }
        const char *rp = end;
        long rad = strtol(rp, &end, 10);
        if (end == rp) rad = STRAIGHT;   // vel only → drive straight, not a 0-radius spin
        r.drive((int16_t)vel, (int16_t)rad);
        out.writeln("ok: drive (raw)");
        return;
    }

    const char *q = nextTok(p);   // token after the direction word
    char *e1;
    long speed = strtol(q, &e1, 10);
    if (e1 == q) speed = 200;     // default speed
    if (speed < 0)   speed = 0;
    if (speed > 500) speed = 500;
    char *e2;
    long ms    = strtol(e1, &e2, 10);
    bool timed = (e2 != e1);
    if (timed && ms > 10000) ms = 10000;   // cap blocking move

    int16_t v = (int16_t)speed, radius = STRAIGHT;
    switch (dir) {
        case 1:  v =  (int16_t)speed; radius = STRAIGHT; break;
        case -1: v = -(int16_t)speed; radius = STRAIGHT; break;
        case 2:  v =  (int16_t)speed; radius = 1;  break;  // spin left (CCW)
        case 3:  v =  (int16_t)speed; radius = -1; break;  // spin right (CW)
    }
    r.drive(v, radius);
    if (timed && ms > 0) {
        self->_port.delay_ms((uint32_t)ms);
        r.stop();
        out.writeln("ok: drive (timed, stopped)");
    } else {
        out.writeln("ok: drive (continuous - 'oi stop' to halt)");
    }
}

inline void RoombaModule::oiCmd(const char *args, Writer &out, void *ctx) {
    RoombaModule *self = static_cast<RoombaModule *>(ctx);
    Roomba &r = self->_roomba;
    const char *p = skipSpaces(args);

    if (*p == '\0' || tokIs(p, "help"))   { usage(out); return; }
    if (tokIs(p, "start"))      { r.start();        out.writeln("ok: start (safe)"); return; }
    if (tokIs(p, "safe"))       { r.safeMode();     out.writeln("ok: safe"); return; }
    if (tokIs(p, "full"))       { r.fullMode();     out.writeln("ok: full"); return; }
    if (tokIs(p, "passive"))    { r.passiveMode();  out.writeln("ok: passive"); return; }
    if (tokIs(p, "wake"))       { out.writeln(r.wake() ? "ok: wake" : "no BRC pin wired"); return; }
    if (tokIs(p, "ping") || tokIs(p, "awake")) {
        // Passive probe — does NOT wake or re-init the base, so it's a true
        // awake/asleep test. Use `oi wake` (or a drive) to actually wake it.
        bool ok = r.ping();
        out.writeln(ok ? "base responding (awake)"
                       : "no response (asleep / OI stopped)");
        return;
    }
    if (tokIs(p, "stop"))       { r.stop();         out.writeln("ok: stop"); return; }
    if (tokIs(p, "clean"))      { r.clean();        out.writeln("ok: clean"); return; }
    if (tokIs(p, "spot"))       { r.spot();         out.writeln("ok: spot"); return; }
    if (tokIs(p, "max"))        { r.maxClean();     out.writeln("ok: max"); return; }
    if (tokIs(p, "dock"))       { r.seekDock();     out.writeln("ok: dock"); return; }
    if (tokIs(p, "power"))      { r.powerDown();    out.writeln("ok: power"); return; }
    if (tokIs(p, "reset"))      { r.reset();        out.writeln("ok: reset"); return; }
    if (tokIs(p, "disconnect")) { r.disconnect();   out.writeln("ok: disconnect"); return; }
    if (tokIs(p, "sensors"))    { dumpSensors(r, out); return; }
    if (tokIs(p, "battery"))    { dumpBattery(r, out); return; }
    if (tokIs(p, "drive"))      { driveCmd(self, nextTok(p), out); return; }

    out.write("unknown oi subcommand: "); out.writeln(p);
    usage(out);
}

inline void RoombaModule::registerCommands(CommandRegistry &reg) {
    reg.registerCommand(CMD("oi", "Roomba OI driver - 'oi' for usage", I2C_NONE, oiCmd, this));
}
