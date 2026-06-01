#!/usr/bin/env python3
"""
find_port.py — find the serial port for a known board by USB VID/PID.

CLI:     find_port.py <board>     # prints the port, exits 0/1 (used by scripts)
Library: from find_port import find_for_project
         port = find_for_project()  # reads the board from the nearest cmdr.toml

Host tools (e.g. bin/irmap.py) call find_for_project() so they pick the same
board as the monitor/upload scripts even when several USB serial devices are
attached.
"""

import sys
from pathlib import Path
from serial.tools import list_ports

BOARDS = {
    "uno": [
        (0x2341, 0x0043),  # Arduino SA, Uno R3 (ATmega16U2)
        (0x2341, 0x0001),  # Arduino SA, Uno (older)
        (0x1A86, 0x7523),  # CH340 clone
    ],
    "pico": [
        (0x2E8A, 0x000A),  # Pico W (RP2040) CDC runtime
    ],
    "pico-boot": [
        (0x2E8A, 0x0003),  # Pico in BOOTSEL mass storage mode
    ],
    "pico2": [
        (0x2E8A, 0x0009),  # Pico 2 W (RP2350) CDC runtime
    ],
    "pico2-boot": [
        (0x2E8A, 0x000F),  # Pico 2 / Pico 2 W in BOOTSEL mass storage mode
    ],
    "r4": [
        (0x2341, 0x006D),  # Arduino Uno R4 WiFi (CDC runtime)
        (0x2341, 0x1002),  # Arduino Uno R4 WiFi (bootloader)
    ],
    "esp32": [
        (0x10C4, 0xEA60),  # Silicon Labs CP2102
        (0x1A86, 0x7523),  # CH340
        (0x0403, 0x6001),  # FTDI FT232R
    ],
    "esp32s3": [
        (0x303A, 0x1001),  # Espressif native USB CDC (S3 built-in USB)
        (0x1A86, 0x55D4),  # CH343P (common on S3 dev boards)
        (0x10C4, 0xEA60),  # Silicon Labs CP2102N
        (0x1A86, 0x7523),  # CH340
    ],
    "bluepill": [
        (0x0483, 0x5740),  # STM32F103 native USB CDC (our TinyUSB app console)
    ],
    "bluepill-dfu": [
        (0xDEAD, 0xCA5D),  # davidgfnet DFU bootloader (for dfu-util)
    ],
}

# cmdr.toml `target` → find_port board key.
TARGET_TO_BOARD = {
    "uno": "uno", "r4": "r4",
    "pico": "pico", "pico2": "pico2",
    "esp32": "esp32s3",
    "bluepill": "bluepill",   # USB-CDC console (USART path uses a manual port)
}


def ports_for(board):
    """All connected ports matching `board`'s VID/PID list (empty if none,
    None if the board is unknown)."""
    targets = BOARDS.get(board)
    if targets is None:
        return None
    return [p.device for p in list_ports.comports()
            for vid, pid in targets if p.vid == vid and p.pid == pid]


def _target_from_cmdr_toml(start):
    """Walk up from `start` looking for cmdr.toml; return its `target` or None."""
    for cand in [Path(start).resolve(), *Path(start).resolve().parents]:
        toml = cand / "cmdr.toml"
        if toml.exists():
            for line in toml.read_text().splitlines():
                s = line.strip()
                if s.startswith("target"):
                    return s.partition("=")[2].strip().strip('"')
            return None
    return None


def find_for_project(start=None):
    """Serial port for the current cmdr project's board (from cmdr.toml), or
    None. Same VID/PID detection the monitor/upload scripts use."""
    target = _target_from_cmdr_toml(start or Path.cwd())
    if not target:
        return None
    matches = ports_for(TARGET_TO_BOARD.get(target, target)) or []
    return matches[0] if matches else None


def find(board):
    """CLI helper: print the matching port or exit with a message."""
    matches = ports_for(board)
    if matches is None:
        print(f"Unknown board '{board}'. Known boards: {', '.join(BOARDS)}", file=sys.stderr)
        sys.exit(1)
    if not matches:
        ids = ", ".join(f"{v:04X}:{p:04X}" for v, p in BOARDS[board])
        print(f"No {board} found (VID:PID candidates: {ids})", file=sys.stderr)
        sys.exit(1)
    if len(matches) > 1:
        print(f"Multiple {board} found: {matches} — using {matches[0]}", file=sys.stderr)
    print(matches[0])


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <board>", file=sys.stderr)
        sys.exit(1)
    find(sys.argv[1])
