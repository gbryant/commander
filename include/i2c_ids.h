#pragma once
#include <stdint.h>

// Pack module ID (upper nibble) + command index (lower nibble) into one byte.
// 16 modules x 15 commands each; 0xF in either nibble is reserved.
// This file is the canonical protocol spec — keep identical across all platforms.
#define I2C_CMD(mod, cmd)  ((uint8_t)(((mod) << 4) | ((cmd) & 0x0F)))
#define I2C_NONE           ((uint8_t)0xFF)  // text-only command, no I2C dispatch

// Module IDs — upper nibble, 0x0–0xE; 0xF reserved
#define MOD_SYSTEM         0x0
#define MOD_LOCOMOTION     0x1
#define MOD_COMPASS        0x2
#define MOD_BLUETOOTH      0x3
#define MOD_I2C            0x4
#define MOD_IR             0x5
#define MOD_SONAR          0x6

// System commands
#define CMD_HELP           I2C_CMD(MOD_SYSTEM, 0x0)
#define CMD_DISCONNECT     I2C_CMD(MOD_SYSTEM, 0x1)
#define CMD_RESET          I2C_CMD(MOD_SYSTEM, 0x2)
#define CMD_BOOTLOADER     I2C_CMD(MOD_SYSTEM, 0x3)

// Compass commands
#define CMD_COMPASS_HEADING I2C_CMD(MOD_COMPASS, 0x0)
#define CMD_COMPASS_RAW     I2C_CMD(MOD_COMPASS, 0x1)

// I2C utility commands
#define CMD_I2C_SCAN       I2C_CMD(MOD_I2C, 0x0)

// IR commands
#define CMD_IR_RECV        I2C_CMD(MOD_IR, 0x0)
#define CMD_IR_WALL        I2C_CMD(MOD_IR, 0x1)

// Sonar commands
#define CMD_SONAR_PING     I2C_CMD(MOD_SONAR, 0x0)
