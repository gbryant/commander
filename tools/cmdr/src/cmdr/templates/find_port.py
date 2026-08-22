#!/usr/bin/env python3
"""
find_port.py — find the serial port for a known board by USB VID/PID.

CLI:     find_port.py <board>     # prints the port; exit 0 ok / 1 absent / 2 ambiguous
Library: from find_port import find_for_project
         port = find_for_project()  # reads the board from the nearest cmdr.toml

Host tools (e.g. bin/irmap.py) call find_for_project() so they pick the same
board as the monitor/upload scripts even when several USB serial devices are
attached.

Two boards can share a USB-serial chip (two CH340s, say), and then VID/PID
cannot tell them apart — so matching is ambiguous and picking one silently
would connect you to the wrong board. When that happens the CLI stops with
exit code 2 and lists the candidates. Disambiguate either way:

    CMDR_PORT=/dev/cu.usbserial-1430 ./monitor    # one-off, machine-specific

    # cmdr.toml — persistent. Prefer `serial`: it identifies the physical
    # board, so it stays correct across machines and re-plugs.
    serial = "5&1D2B3C4&0&2"
    port   = "/dev/cu.usbserial-1430"
"""

import os
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


def _matching(board):
    """ListPortInfo objects matching `board`'s VID/PID list (None if unknown)."""
    targets = BOARDS.get(board)
    if targets is None:
        return None
    return [p for p in list_ports.comports()
            for vid, pid in targets if p.vid == vid and p.pid == pid]


def ports_for(board):
    """Device paths matching `board` (empty if none, None if board unknown)."""
    matches = _matching(board)
    return None if matches is None else [p.device for p in matches]


def _cmdr_toml_keys(start):
    """Top-level key/value pairs from the nearest cmdr.toml (before any
    [section] header), as a dict. Empty if there's no cmdr.toml."""
    for cand in [Path(start).resolve(), *Path(start).resolve().parents]:
        toml = cand / "cmdr.toml"
        if toml.exists():
            keys = {}
            for line in toml.read_text().splitlines():
                s = line.strip()
                if s.startswith("["):
                    break                      # into [autostart] etc. — stop
                if "=" in s and not s.startswith("#"):
                    k, _, v = s.partition("=")
                    keys[k.strip()] = v.strip().strip('"')
            return keys
    return {}


def _pinned(matches, start=None):
    """Resolve an explicit pin to one of `matches`: $CMDR_PORT, else cmdr.toml
    `serial` (preferred — identifies the board itself) or `port`. Returns the
    device path, or None if nothing is pinned.

    A pin is honoured even when it names a port we didn't match, so an
    unrecognised VID/PID can still be driven by setting CMDR_PORT."""
    env = os.environ.get("CMDR_PORT")
    if env:
        return env
    keys = _cmdr_toml_keys(start or Path.cwd())
    if keys.get("serial"):
        for p in matches:
            if p.serial_number == keys["serial"]:
                return p.device
    if keys.get("port"):
        return keys["port"]
    return None


def _ambiguity_report(board, matches):
    """Human-readable 'which one did you mean' message.

    Which pin we recommend depends on the hardware: only some USB-serial chips
    report a serial number (CH340 clones typically do not), and `serial` is
    useless without one — so fall back to recommending the port path."""
    lines = [f"Ambiguous: {len(matches)} devices match '{board}' — refusing to guess.",
             "  (boards sharing a USB-serial chip are indistinguishable by VID/PID)"]
    for p in matches:
        bits = [f"    {p.device}"]
        if p.serial_number:
            bits.append(f"serial={p.serial_number}")
        if p.location:
            bits.append(f"usb={p.location}")
        if p.description:
            bits.append(p.description)
        lines.append("   ".join(bits))
    lines += ["",
              "  Unplug one and re-run to see which is which.",
              "",
              "  Pick one for a single run:",
              f"    CMDR_PORT={matches[0].device} <command>",
              "",
              "  Or pin it in cmdr.toml:"]
    if any(p.serial_number for p in matches):
        sn = next(p.serial_number for p in matches if p.serial_number)
        lines += [f'    serial = "{sn}"      # preferred — follows the board itself',
                  f'    port   = "{matches[0].device}"   # or pin the path']
    else:
        lines += [f'    port   = "{matches[0].device}"',
                  "    (these devices report no serial number, so `serial` can't",
                  "     identify them — the path is the only stable handle, and it",
                  "     may change if you move the board to another USB port)"]
    return "\n".join(lines)


def find_for_project(start=None):
    """Serial port for the current cmdr project's board (from cmdr.toml), or
    None. Same detection the monitor/upload scripts use. On ambiguity this
    warns on stderr and returns the first match, so interactive host tools keep
    working — the CLI below is the strict one."""
    keys = _cmdr_toml_keys(start or Path.cwd())
    target = keys.get("target")
    if not target:
        return None
    matches = _matching(TARGET_TO_BOARD.get(target, target)) or []
    pin = _pinned(matches, start)
    if pin:
        return pin
    if not matches:
        return None
    if len(matches) > 1:
        print(_ambiguity_report(target, matches), file=sys.stderr)
        print(f"  → using {matches[0].device}", file=sys.stderr)
    return matches[0].device


def find(board):
    """CLI helper: print the matching port or exit.

    Exit codes are meaningful to the dev scripts: 1 means 'not present yet'
    (they poll on it while a board re-enumerates), 2 means 'present but
    ambiguous' — a real problem that polling will never resolve."""
    matches = _matching(board)
    if matches is None:
        print(f"Unknown board '{board}'. Known boards: {', '.join(BOARDS)}", file=sys.stderr)
        sys.exit(2)
    pin = _pinned(matches)
    if pin:
        print(pin)
        return
    if not matches:
        ids = ", ".join(f"{v:04X}:{p:04X}" for v, p in BOARDS[board])
        print(f"No {board} found (VID:PID candidates: {ids})", file=sys.stderr)
        sys.exit(1)
    if len(matches) > 1:
        print(_ambiguity_report(board, matches), file=sys.stderr)
        sys.exit(2)
    print(matches[0].device)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <board>", file=sys.stderr)
        sys.exit(1)
    find(sys.argv[1])
