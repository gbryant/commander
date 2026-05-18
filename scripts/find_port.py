#!/usr/bin/env python3
"""
find_port.py — find the serial port for a known board by USB VID/PID.
Usage: find_port.py <board>
Prints the port path and exits 0, or prints to stderr and exits 1.
"""

import sys
from serial.tools import list_ports

BOARDS = {
    "uno": [
        (0x2341, 0x0043),  # Arduino SA, Uno R3 (ATmega16U2)
        (0x2341, 0x0001),  # Arduino SA, Uno (older)
        (0x1A86, 0x7523),  # CH340 clone
    ],
    "pico": [
        (0x2E8A, 0x000A),  # Pico / Pico W running CDC firmware
    ],
    "pico-boot": [
        (0x2E8A, 0x0003),  # Pico in BOOTSEL mass storage mode
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
}


def find(board):
    targets = BOARDS.get(board)
    if targets is None:
        known = ", ".join(BOARDS)
        print(f"Unknown board '{board}'. Known boards: {known}", file=sys.stderr)
        sys.exit(1)

    matches = []
    for port in list_ports.comports():
        for vid, pid in targets:
            if port.vid == vid and port.pid == pid:
                matches.append(port.device)

    if not matches:
        ids = ", ".join(f"{v:04X}:{p:04X}" for v, p in targets)
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
