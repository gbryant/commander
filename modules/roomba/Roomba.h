#pragma once
#include <stdint.h>
#include <stddef.h>

// Portable iRobot Open Interface (OI) driver.
//
// Pure protocol — no Arduino, no HardwareSerial, no HAL. The driver talks to the
// robot only through an abstract RoombaPort (byte I/O + timing + optional BRC
// wake line), so the same code drives a Roomba from any board; each platform
// provides a small RoombaPort adapter (e.g. Arduino R4: wrap Serial1 on D0/D1).
//
// Ported from robot-framework/src/Roomba.h, preserving its hard-won fixes:
//   - enter SAFE mode immediately after START (Passive ignores motor commands)
//   - drive-straight radius 0x8000 overflow handling
//   - per-byte 150ms timeout when reading sensor replies (inter-byte gaps)
//   - cast to uint16_t before shifting (avoid sign extension)
//   - readAllSensors via Packet Group 0 (26 bytes in one request)
//   - BRC/DD pin wake sequence (Mini-DIN pin 5)

// ── OI command opcodes ───────────────────────────────────────────────────────
#define OI_START        128  // 0x80 - Start OI (enters Passive)
#define OI_RESET        7    // 0x07 - Reset
#define OI_STOP         173  // 0xAD - Stop OI
#define OI_BAUD         129  // 0x81 - Set baud rate
#define OI_SAFE         131  // 0x83 - Safe mode
#define OI_FULL         132  // 0x84 - Full mode
#define OI_CLEAN        135  // 0x87 - Clean
#define OI_MAX          136  // 0x88 - Max clean
#define OI_SPOT         134  // 0x86 - Spot clean
#define OI_SEEK_DOCK    143  // 0x8F - Seek dock
#define OI_POWER        133  // 0x85 - Power down
#define OI_DRIVE        137  // 0x89 - Drive (velocity + radius)
#define OI_DRIVE_DIRECT 145  // 0x91 - Drive (individual wheel velocities)
#define OI_MOTORS       138  // 0x8A - Control motors
#define OI_LEDS         139  // 0x8B - Control LEDs
#define OI_SONG         140  // 0x8C - Define song
#define OI_PLAY         141  // 0x8D - Play song
#define OI_SENSORS      142  // 0x8E - Request sensor data

// ── Sensor packet IDs ────────────────────────────────────────────────────────
#define SENSOR_GROUP_0           0   // packets 7-26 (26 bytes)
#define SENSOR_GROUP_1           1
#define SENSOR_GROUP_2           2
#define SENSOR_GROUP_3           3
#define SENSOR_GROUP_6           6
#define SENSOR_BUMPS_DROPS       7
#define SENSOR_WALL              8
#define SENSOR_CLIFF_LEFT        9
#define SENSOR_CLIFF_FRONT_LEFT  10
#define SENSOR_CLIFF_FRONT_RIGHT 11
#define SENSOR_CLIFF_RIGHT       12
#define SENSOR_VIRTUAL_WALL      13
#define SENSOR_OVERCURRENTS      14
#define SENSOR_DIRT_DETECT       15
#define SENSOR_INFRARED_CHAR     17
#define SENSOR_BUTTONS           18
#define SENSOR_DISTANCE          19
#define SENSOR_ANGLE             20
#define SENSOR_CHARGING_STATE    21
#define SENSOR_VOLTAGE           22
#define SENSOR_CURRENT           23
#define SENSOR_TEMPERATURE       24
#define SENSOR_BATTERY_CHARGE    25
#define SENSOR_BATTERY_CAPACITY  26

// ── Charging states ──────────────────────────────────────────────────────────
#define CHARGE_NOT_CHARGING      0
#define CHARGE_RECONDITIONING    1
#define CHARGE_FULL_CHARGING     2
#define CHARGE_TRICKLE_CHARGING  3
#define CHARGE_WAITING           4
#define CHARGE_FAULT             5

// ── Decoded sensor snapshot ──────────────────────────────────────────────────
struct RoombaSensors {
    // Bumps and wheel drops (packet 7)
    bool bumpRight;
    bool bumpLeft;
    bool wheelDropRight;
    bool wheelDropLeft;
    bool wheelDropCaster;

    // Cliffs / walls
    bool cliffLeft;
    bool cliffFrontLeft;
    bool cliffFrontRight;
    bool cliffRight;
    bool wall;
    bool virtualWall;

    // Buttons
    bool buttonClock;
    bool buttonSchedule;
    bool buttonDay;
    bool buttonHour;
    bool buttonMinute;
    bool buttonDock;
    bool buttonSpot;
    bool buttonClean;

    // Movement
    int16_t distance;   // mm since last read
    int16_t angle;      // degrees since last read

    // Battery
    uint8_t  chargingState;
    uint16_t voltage;         // mV
    int16_t  current;         // mA
    int8_t   temperature;     // deg C
    uint16_t batteryCharge;   // mAh
    uint16_t batteryCapacity; // mAh

    // Misc
    uint8_t dirtDetect;
    uint8_t infraredChar;
    bool    overcurrentSideBrush;
    bool    overcurrentMainBrush;
    bool    overcurrentRightWheel;
    bool    overcurrentLeftWheel;
};

// ── Platform port the driver depends on ──────────────────────────────────────
// Implement this per platform. The serial link must already be open at the OI
// baud rate (115200 for Create 2 / 600-series; 19200 for older robots) before
// the Roomba driver is used.
class RoombaPort {
public:
    virtual ~RoombaPort() = default;

    // Serial I/O to the robot.
    virtual void write(const uint8_t *data, size_t len) = 0;
    virtual int  read()      = 0;   // next RX byte, or -1 if none available
    virtual int  available() = 0;   // bytes waiting in the RX buffer

    // Timing. delay_ms should yield to other tasks (vTaskDelay), not busy-spin.
    virtual uint32_t now_ms()           = 0;
    virtual void     delay_ms(uint32_t) = 0;

    // Optional BRC/Device-Detect line (Mini-DIN pin 5) for waking from sleep.
    // Default: no BRC wired — wake() becomes a no-op.
    virtual bool has_brc() const        { return false; }
    virtual void set_brc(bool /*high*/) {}
};

// ── OI driver ────────────────────────────────────────────────────────────────
class Roomba {
public:
    Roomba() : port(nullptr), initialized(false), _chargingMode(false) {}

    // Bind to an already-open port. The platform opens the serial link at the
    // OI baud rate before calling this.
    void begin(RoombaPort &p) {
        port = &p;
        initialized = false;
        if (port->has_brc()) port->set_brc(true);  // idle high
        port->delay_ms(100);                        // let the robot settle
    }

    // Wake the robot from sleep via the BRC/DD line. No-op if no BRC wired.
    bool wake() {
        if (!port || !port->has_brc()) return false;
        port->set_brc(false);
        port->delay_ms(500);
        port->set_brc(true);
        port->delay_ms(2000);  // fully awake before any OI command
        return true;
    }

    // Start the OI and enter Safe Mode. Motor commands are ignored in Passive
    // (the state OI_START leaves you in), so we switch to Safe immediately.
    bool start() {
        if (!port) return false;
        wake();                       // no-op if BRC not wired
        writeByte(OI_START);
        port->delay_ms(20);
        writeByte(OI_SAFE);
        port->delay_ms(20);
        initialized = true;
        return true;
    }

    bool reset() {
        if (!port) return false;
        writeByte(OI_RESET);
        initialized = false;
        port->delay_ms(5000);
        return true;
    }

    bool safeMode() {
        if (!ensureStarted()) return false;
        writeByte(OI_SAFE);
        port->delay_ms(20);
        return true;
    }

    bool fullMode() {
        if (!ensureStarted()) return false;
        writeByte(OI_FULL);
        port->delay_ms(20);
        return true;
    }

    bool clean()    { return simpleCmd(OI_CLEAN); }
    bool spot()     { return simpleCmd(OI_SPOT); }
    bool maxClean() { return simpleCmd(OI_MAX); }
    bool seekDock() { return simpleCmd(OI_SEEK_DOCK); }

    // Enter Passive: OI stays connected, motors stop, charging is allowed.
    bool passiveMode() {
        if (!port) return false;
        writeByte(OI_START);
        port->delay_ms(20);
        initialized = true;
        return true;
    }

    void enableChargingMode()  { passiveMode(); _chargingMode = true; }
    void disableChargingMode() { _chargingMode = false; }
    bool isChargingMode() const { return _chargingMode; }

    bool isOnDock(const RoombaSensors &s) const {
        return s.chargingState == CHARGE_RECONDITIONING ||
               s.chargingState == CHARGE_FULL_CHARGING ||
               s.chargingState == CHARGE_TRICKLE_CHARGING;
    }

    // Stop the OI entirely — robot returns to normal (non-OI) operation.
    bool disconnect() {
        if (!port) return false;
        writeByte(OI_STOP);
        port->delay_ms(20);
        initialized = false;
        _chargingMode = false;
        return true;
    }

    // Stop motion (drive at zero velocity, straight).
    bool stop() {
        if (!ensureStarted()) return false;
        uint8_t cmd[5] = {OI_DRIVE, 0x00, 0x00, 0x80, 0x00};
        port->write(cmd, 5);
        port->delay_ms(20);
        return true;
    }

    // Drive with velocity (-500..500 mm/s) and radius (-2000..2000 mm).
    // Special radius codes: 0x8000/0x7FFF = straight, 0xFFFF = spin CW, 1 = CCW.
    bool drive(int16_t velocity, int16_t radius) {
        if (!ensureStarted()) return false;
        if (velocity >  500) velocity =  500;
        if (velocity < -500) velocity = -500;

        // 0x8000 ("drive straight") is -32768 as int16_t — compare against the
        // cast value so we don't clamp the special code as an out-of-range radius.
        if (radius != (int16_t)0x8000 && radius != 32767 && radius != -1 && radius != 1) {
            if (radius >  2000) radius =  2000;
            if (radius < -2000) radius = -2000;
        }

        uint8_t cmd[5];
        cmd[0] = OI_DRIVE;
        cmd[1] = (uint8_t)((velocity >> 8) & 0xFF);
        cmd[2] = (uint8_t)(velocity & 0xFF);
        cmd[3] = (uint8_t)((radius >> 8) & 0xFF);
        cmd[4] = (uint8_t)(radius & 0xFF);
        port->write(cmd, 5);
        port->delay_ms(20);
        return true;
    }

    // Drive each wheel independently (-500..500 mm/s).
    bool driveDirect(int16_t rightVelocity, int16_t leftVelocity) {
        if (!ensureStarted()) return false;
        if (rightVelocity >  500) rightVelocity =  500;
        if (rightVelocity < -500) rightVelocity = -500;
        if (leftVelocity  >  500) leftVelocity  =  500;
        if (leftVelocity  < -500) leftVelocity  = -500;

        uint8_t cmd[5];
        cmd[0] = OI_DRIVE_DIRECT;
        cmd[1] = (uint8_t)((rightVelocity >> 8) & 0xFF);
        cmd[2] = (uint8_t)(rightVelocity & 0xFF);
        cmd[3] = (uint8_t)((leftVelocity >> 8) & 0xFF);
        cmd[4] = (uint8_t)(leftVelocity & 0xFF);
        port->write(cmd, 5);
        port->delay_ms(20);
        return true;
    }

    bool powerDown() {
        if (!ensureStarted()) return false;
        writeByte(OI_POWER);
        initialized = false;
        port->delay_ms(20);
        return true;
    }

    bool isInitialized() const { return initialized; }

    // True if a BRC/Device-Detect wake line is wired (Mini-DIN 5). Lets callers
    // decide whether it's safe to let the robot sleep (only if they can wake it).
    bool hasBrc() const { return port && port->has_brc(); }

    // Passive responsiveness probe: send a raw sensor query and report whether the
    // OI answers. Deliberately does NOT ensureStarted()/wake()/pulse BRC, so it's a
    // true awake/asleep test — a sleeping base (or one with the OI stopped) stays
    // silent and this returns false. (Use drive()/start()/wake() to actually wake.)
    bool ping() {
        if (!port) return false;
        while (port->available()) port->read();        // flush stale RX
        uint8_t cmd[2] = {OI_SENSORS, SENSOR_GROUP_0};
        port->write(cmd, 2);
        uint32_t startMs = port->now_ms();
        while (port->now_ms() - startMs < 200) {
            if (port->available()) return true;        // got a reply → awake, OI active
            port->delay_ms(2);
        }
        return false;                                  // silent → asleep / OI stopped
    }

    // ── Sensors ──────────────────────────────────────────────────────────────

    // Request one sensor packet and read `length` reply bytes. Uses a per-byte
    // timeout (not a single deadline) to tolerate inter-byte gaps from the robot.
    bool readSensor(uint8_t packetId, uint8_t *buffer, uint8_t length) {
        if (!ensureStarted()) return false;

        while (port->available()) port->read();   // flush stale RX

        uint8_t cmd[2] = {OI_SENSORS, packetId};
        port->write(cmd, 2);

        uint32_t lastByte = port->now_ms();
        uint8_t  n = 0;
        while (n < length) {
            if (port->available()) {
                buffer[n++] = (uint8_t)port->read();
                lastByte = port->now_ms();
            } else if (port->now_ms() - lastByte > 150) {
                return false;                       // inter-byte timeout
            } else {
                port->delay_ms(1);                  // yield while waiting
            }
        }
        return true;
    }

    // Read packets 7-26 in one Group-0 request (fast and reliable) and decode.
    bool readAllSensors(RoombaSensors &s) {
        const uint8_t PACKET_0_SIZE = 26;
        uint8_t b[PACKET_0_SIZE];
        if (!readSensor(SENSOR_GROUP_0, b, PACKET_0_SIZE)) return false;

        s.bumpRight       = b[0] & 0x01;
        s.bumpLeft        = b[0] & 0x02;
        s.wheelDropRight  = b[0] & 0x04;
        s.wheelDropLeft   = b[0] & 0x08;
        s.wheelDropCaster = b[0] & 0x10;

        s.wall           = b[1] & 0x01;
        s.cliffLeft      = b[2] & 0x01;
        s.cliffFrontLeft = b[3] & 0x01;
        s.cliffFrontRight= b[4] & 0x01;
        s.cliffRight     = b[5] & 0x01;
        s.virtualWall    = b[6] & 0x01;

        s.overcurrentSideBrush  = b[7] & 0x01;
        s.overcurrentMainBrush  = b[7] & 0x04;
        s.overcurrentRightWheel = b[7] & 0x08;
        s.overcurrentLeftWheel  = b[7] & 0x10;

        s.dirtDetect   = b[8];
        // b[9] reserved
        s.infraredChar = b[10];

        s.buttonClean    = b[11] & 0x01;
        s.buttonSpot     = b[11] & 0x02;
        s.buttonDock     = b[11] & 0x04;
        s.buttonMinute   = b[11] & 0x08;
        s.buttonHour     = b[11] & 0x10;
        s.buttonDay      = b[11] & 0x20;
        s.buttonSchedule = b[11] & 0x40;
        s.buttonClock    = b[11] & 0x80;

        s.distance        = (int16_t)(((uint16_t)b[12] << 8) | b[13]);
        s.angle           = (int16_t)(((uint16_t)b[14] << 8) | b[15]);
        s.chargingState   = b[16];
        s.voltage         = (uint16_t)(((uint16_t)b[17] << 8) | b[18]);
        s.current         = (int16_t)(((uint16_t)b[19] << 8) | b[20]);
        s.temperature     = (int8_t)b[21];
        s.batteryCharge   = (uint16_t)(((uint16_t)b[22] << 8) | b[23]);
        s.batteryCapacity = (uint16_t)(((uint16_t)b[24] << 8) | b[25]);
        return true;
    }

    uint8_t getBatteryPercent(const RoombaSensors &s) const {
        if (s.batteryCapacity == 0) return 0;
        uint32_t pct = ((uint32_t)s.batteryCharge * 100) / s.batteryCapacity;
        return pct > 100 ? 100 : (uint8_t)pct;
    }

    const char *getChargingStateString(uint8_t state) const {
        switch (state) {
            case CHARGE_NOT_CHARGING:     return "Not charging";
            case CHARGE_RECONDITIONING:   return "Reconditioning";
            case CHARGE_FULL_CHARGING:    return "Full charging";
            case CHARGE_TRICKLE_CHARGING: return "Trickle charging";
            case CHARGE_WAITING:          return "Waiting";
            case CHARGE_FAULT:            return "Charging fault";
            default:                      return "Unknown";
        }
    }

private:
    RoombaPort *port;
    bool        initialized;
    bool        _chargingMode;

    void writeByte(uint8_t b) { port->write(&b, 1); }

    bool simpleCmd(uint8_t opcode) {
        if (!ensureStarted()) return false;
        writeByte(opcode);
        port->delay_ms(20);
        return true;
    }

    bool ensureStarted() {
        if (!port) return false;
        return initialized ? true : start();
    }
};
