#pragma once
#include <stdint.h>
#include <stddef.h>
#include "i2c_ids.h"

// Wire format for the generic locomotion link (Pico master ↔ R4 bridge).
//
// Pure protocol — no Arduino, no HAL, no Pico SDK. Both ends include this so the
// byte layout can never drift: the master packs, the bridge unpacks, and vice
// versa for the sensor snapshot. The I2C "register" is the command byte from
// i2c_ids.h (CMD_LOCO_*), so there is no extra framing — the payloads below are
// what travels after that byte.
//
// Velocity/radius use iRobot OI semantics (mm/s, mm) because Roomba is today's
// base; a future base reuses the same numbers. All multi-byte fields are
// big-endian (matches the OI drive packet, so the bridge can forward verbatim).

// ── Drive radius special codes (OI-compatible) ───────────────────────────────
// 0x8000 ("drive straight") is -32768 as int16_t; compare against the cast value.
#define LOCO_RADIUS_STRAIGHT  ((int16_t)0x8000)
#define LOCO_RADIUS_CW        ((int16_t)-1)   // spin clockwise in place
#define LOCO_RADIUS_CCW       ((int16_t)1)    // spin counter-clockwise in place

// ── CMD_LOCO_DRIVE payload: 4 bytes, big-endian ──────────────────────────────
//   [vel_hi][vel_lo][rad_hi][rad_lo]
#define LOCO_DRIVE_LEN  4

static inline void loco_pack_drive(int16_t vel, int16_t radius, uint8_t out[LOCO_DRIVE_LEN]) {
    out[0] = (uint8_t)((vel >> 8) & 0xFF);
    out[1] = (uint8_t)(vel & 0xFF);
    out[2] = (uint8_t)((radius >> 8) & 0xFF);
    out[3] = (uint8_t)(radius & 0xFF);
}

static inline void loco_unpack_drive(const uint8_t in[LOCO_DRIVE_LEN], int16_t *vel, int16_t *radius) {
    *vel    = (int16_t)(((uint16_t)in[0] << 8) | in[1]);
    *radius = (int16_t)(((uint16_t)in[2] << 8) | in[3]);
}

// ── CMD_LOCO_SENSORS payload: fixed snapshot, big-endian ─────────────────────
// Generic mobile-base telemetry (not Roomba-specific). The bridge fills this
// from whatever base it drives; the master decodes it the same way regardless.
#define LOCO_SENSORS_LEN  12

// flags1 bit assignments (byte 0)
#define LOCO_F1_BUMP_LEFT    0x01
#define LOCO_F1_BUMP_RIGHT   0x02
#define LOCO_F1_DROP_LEFT    0x04
#define LOCO_F1_DROP_RIGHT   0x08
#define LOCO_F1_DROP_CASTER  0x10
#define LOCO_F1_WALL         0x20
#define LOCO_F1_VWALL        0x40
// flags2 bit assignments (byte 1) — cliff sensors
#define LOCO_F2_CLIFF_LEFT   0x01
#define LOCO_F2_CLIFF_FL     0x02
#define LOCO_F2_CLIFF_FR     0x04
#define LOCO_F2_CLIFF_RIGHT  0x08

// Decoded snapshot. The pack/unpack helpers below are the single source of truth
// for the byte order; callers should never index the wire buffer by hand.
struct LocoSensors {
    uint8_t  flags1;          // LOCO_F1_*
    uint8_t  flags2;          // LOCO_F2_*
    int16_t  distance_mm;     // since last read
    int16_t  angle_deg;       // since last read
    uint8_t  battery_pct;     // 0..100
    uint8_t  charging_state;  // base-defined enum (Roomba: CHARGE_*)
    uint16_t voltage_mv;
    int16_t  current_ma;
};

static inline void loco_pack_sensors(const LocoSensors *s, uint8_t out[LOCO_SENSORS_LEN]) {
    out[0]  = s->flags1;
    out[1]  = s->flags2;
    out[2]  = (uint8_t)((s->distance_mm >> 8) & 0xFF);
    out[3]  = (uint8_t)(s->distance_mm & 0xFF);
    out[4]  = (uint8_t)((s->angle_deg >> 8) & 0xFF);
    out[5]  = (uint8_t)(s->angle_deg & 0xFF);
    out[6]  = s->battery_pct;
    out[7]  = s->charging_state;
    out[8]  = (uint8_t)((s->voltage_mv >> 8) & 0xFF);
    out[9]  = (uint8_t)(s->voltage_mv & 0xFF);
    out[10] = (uint8_t)((s->current_ma >> 8) & 0xFF);
    out[11] = (uint8_t)(s->current_ma & 0xFF);
}

static inline void loco_unpack_sensors(const uint8_t in[LOCO_SENSORS_LEN], LocoSensors *s) {
    s->flags1         = in[0];
    s->flags2         = in[1];
    s->distance_mm    = (int16_t)(((uint16_t)in[2] << 8) | in[3]);
    s->angle_deg      = (int16_t)(((uint16_t)in[4] << 8) | in[5]);
    s->battery_pct    = in[6];
    s->charging_state = in[7];
    s->voltage_mv     = (uint16_t)(((uint16_t)in[8] << 8) | in[9]);
    s->current_ma     = (int16_t)(((uint16_t)in[10] << 8) | in[11]);
}
