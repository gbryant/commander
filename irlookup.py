#!/usr/bin/env python3
"""
irlookup.py — Real-time IR button lookup against all saved map files.

Loads every JSON file from the maps directory, activates recv mode, and
identifies each button press — showing which remote(s) and button name(s)
match, or reporting it as unknown with the raw signal data.  When the same
IR code appears in more than one map file the matches are listed together,
making it easy to spot buttons that are shared across remotes.

Usage:
    python3 irlookup.py [PORT] [--maps DIR]

    PORT       Serial port (auto-detected if omitted)
    --maps DIR Directory of JSON map files (default: maps/)

Requires: pip install pyserial
"""

import argparse
import json
import re
import sys
import time
from pathlib import Path

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("pyserial not found.  Run: pip install pyserial")

BAUD         = 115200
DEFAULT_MAPS = "maps"

IR_RE = re.compile(
    r'Protocol=(\w+)\s+Address=(0x[0-9A-Fa-f]+),\s+'
    r'Command=(0x[0-9A-Fa-f]+),\s+Raw-Data=(0x[0-9A-Fa-f]+),\s+'
    r'(\d+)\s+bits'
)


# ---------------------------------------------------------------------------
# Serial helpers (shared with irmap.py)
# ---------------------------------------------------------------------------

def find_port():
    for p in serial.tools.list_ports.comports():
        if p.device and Path(p.device).name.startswith('cu.usb'):
            return p.device
    return None


def send(ser, text):
    ser.write((text + '\r').encode())


def drain_until(ser, marker, timeout=3.0):
    deadline = time.time() + timeout
    buf = ''
    while time.time() < deadline:
        chunk = ser.read(ser.in_waiting or 1).decode('utf-8', errors='replace')
        buf += chunk
        if marker in buf:
            return True
    return False


# ---------------------------------------------------------------------------
# Map loading and lookup
# ---------------------------------------------------------------------------

def load_maps(maps_dir):
    """Return dict: remote_name -> list of entries, sorted by filename."""
    maps = {}
    path = Path(maps_dir)
    if not path.exists():
        return maps
    for f in sorted(path.glob('*.json')):
        try:
            entries = json.loads(f.read_text())
            maps[f.stem] = entries
        except Exception as exc:
            print(f"Warning: skipping {f.name}: {exc}", file=sys.stderr)
    return maps


def lookup(maps, address, command):
    """Return list of (remote_name, entry) for every entry matching address+command."""
    results = []
    for remote, entries in maps.items():
        for entry in entries:
            if entry['address'] == address and entry['command'] == command:
                results.append((remote, entry))
    return results


# ---------------------------------------------------------------------------
# Display
# ---------------------------------------------------------------------------

def fmt_match(remote, entry):
    sel = entry.get('select') or entry.get('player')
    s = f"{remote} : {entry['name']}"
    if sel is not None:
        s += f" / {sel}"
    return s


def show(protocol, address, command, bits, matches):
    raw = f"{protocol:<8}  {address:<8}  {command:<6}  {bits}b"
    if not matches:
        print(f"  ?  {raw}")
    elif len(matches) == 1:
        print(f"  ✓  {raw}  →  {fmt_match(*matches[0])}")
    else:
        print(f"  ✓  {raw}  →  {len(matches)} matches")
        for remote, entry in matches:
            print(f"       {fmt_match(remote, entry)}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description='Real-time IR button lookup against saved maps')
    parser.add_argument('port',   nargs='?', help='Serial port (auto-detected if omitted)')
    parser.add_argument('--maps', '-m', default=DEFAULT_MAPS,
                        metavar='DIR', help='Map files directory (default: maps/)')
    args = parser.parse_args()

    port = args.port or find_port()
    if not port:
        sys.exit("No serial port found. Specify one explicitly.")

    # Load maps
    maps = load_maps(args.maps)
    if maps:
        total   = sum(len(v) for v in maps.values())
        summary = '  '.join(f"{name} ({len(e)})" for name, e in maps.items())
        print(f"Loaded {len(maps)} map{'s' if len(maps) != 1 else ''}  [{total} entries]")
        print(f"  {summary}")
    else:
        print(f"No map files found in '{args.maps}/' — reporting all buttons as unknown.")
        print( "  Add JSON files produced by irmap.py to that directory.")

    print()

    # Connect
    print(f"Connecting to {port} at {BAUD} baud...")
    try:
        ser = serial.Serial(port, BAUD, timeout=0.1)
    except serial.SerialException as exc:
        sys.exit(f"Could not open port: {exc}")

    time.sleep(2)
    ser.reset_input_buffer()

    send(ser, 'recv')
    if not drain_until(ser, 'listening'):
        send(ser, 'recv')
        if not drain_until(ser, 'listening'):
            print("Warning: could not confirm recv mode. Continuing anyway.")

    print("Listening — press any button.  Ctrl-C to quit.\n")

    try:
        while True:
            line = ser.readline().decode('utf-8', errors='replace').strip()
            if not line:
                continue

            m = IR_RE.search(line)
            if not m:
                continue

            if 'Repeat' in line:
                continue

            protocol = m.group(1)
            address  = m.group(2).lower()
            command  = m.group(3).lower()
            bits     = int(m.group(5))

            matches = lookup(maps, address, command)
            show(protocol, address, command, bits, matches)

    except KeyboardInterrupt:
        print("\nDone.")

    finally:
        try:
            send(ser, 'recv')
            time.sleep(0.2)
            ser.close()
        except Exception:
            pass


if __name__ == '__main__':
    main()
