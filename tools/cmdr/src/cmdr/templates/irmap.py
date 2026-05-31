#!/usr/bin/env python3
"""
irmap.py — Interactive IR button mapper for nano-commander.

Connects to the device, activates recv mode, and guides you through
pressing each button to build a named JSON map.

Usage:
    python3 irmap.py [PORT] [--output FILE] [--selects NAME ...]

    PORT              Serial port (auto-detected if omitted)
    --output FILE     Output JSON file (default: ir_map.json)
    --selects NAME…   Names for each position of the select switch, left to
                      right.  Omit entirely for remotes with no select switch.

Examples:
    python3 irmap.py                              # no select switch
    python3 irmap.py --selects cd1 cd2 cd3        # 3-position CD toggle
    python3 irmap.py --selects VTR4 ID            # 2-position video toggle

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

BAUD        = 115200
DEFAULT_OUT = "ir_map.json"

IR_RE = re.compile(
    r'Protocol=(\w+)\s+Address=(0x[0-9A-Fa-f]+),\s+'
    r'Command=(0x[0-9A-Fa-f]+),\s+Raw-Data=(0x[0-9A-Fa-f]+),\s+'
    r'(\d+)\s+bits'
)


# ---------------------------------------------------------------------------
# Serial helpers
# ---------------------------------------------------------------------------

def find_port():
    for p in serial.tools.list_ports.comports():
        if p.device and Path(p.device).name.startswith('cu.usb'):
            return p.device
    return None


def send(ser, text):
    ser.write((text + '\r').encode())


def drain_until(ser, marker, timeout=3.0):
    """Read and discard until marker appears or timeout."""
    deadline = time.time() + timeout
    buf = ''
    while time.time() < deadline:
        chunk = ser.read(ser.in_waiting or 1).decode('utf-8', errors='replace')
        buf += chunk
        if marker in buf:
            return True
    return False


# ---------------------------------------------------------------------------
# Select-switch helpers
# ---------------------------------------------------------------------------

def prompt_select(selects, suggested=None):
    """
    Ask the user which select-switch position this button was pressed on.
    Returns the position name (string) or None if skipped.

    Accepts the position name ("VTR4"), its 1-based index ("1"), or Enter.
    If suggested is provided it is shown as a pre-filled default.
    """
    options_str = '/'.join(selects)
    if suggested is not None:
        raw = input(f"  Select [{suggested}] (- for none): ").strip()
        if not raw:
            return suggested
        if raw == '-':
            return None
    else:
        raw = input(f"  Select ({options_str}, Enter to skip): ").strip()
        if not raw:
            return None

    # Accept a 1-based index
    if raw.isdigit():
        idx = int(raw) - 1
        if 0 <= idx < len(selects):
            return selects[idx]

    # Accept the name directly (case-insensitive)
    for name in selects:
        if raw.lower() == name.lower():
            return name

    return None  # unrecognised — treat as skipped


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description='Map IR remote buttons via nano-commander')
    parser.add_argument('port',       nargs='?',   help='Serial port (auto-detected if omitted)')
    parser.add_argument('--output',   '-o',        default=DEFAULT_OUT, help='Output JSON file')
    parser.add_argument('--selects',  nargs='*',   metavar='NAME',
                        help='Select-switch position names, left to right')
    args = parser.parse_args()

    port     = args.port or find_port()
    out_path = Path(args.output)
    selects  = args.selects or []   # empty list = no select switch

    if not port:
        sys.exit("No serial port found. Specify one explicitly, e.g.: python3 irmap.py /dev/cu.usbmodem14401")

    if selects:
        print(f"Select switch positions: {', '.join(f'{i+1}={n}' for i, n in enumerate(selects))}")

    # Load existing map so the session can be resumed
    entries           = []   # ordered list of saved entries
    seen              = {}   # (address, command) -> entry | None  (None = skipped)
    command_names     = {}   # command -> name,   for cross-select name pre-fill
    address_selects   = {}   # address -> select, for cross-command select pre-fill
    no_select_addresses = set()  # addresses confirmed to have no select (auto-skip)

    if out_path.exists():
        with out_path.open() as f:
            entries = json.load(f)
        for e in entries:
            key = (e['address'], e['command'])
            seen[key] = e
            if e.get('name'):
                command_names[e['command']] = e['name']
            # support both old 'player' key and new 'select' key
            sel = e.get('select') or e.get('player')
            if sel is not None:
                address_selects[e['address']] = str(sel)
            elif 'select' in e and e['select'] is None:
                # explicitly null — this address doesn't use the select switch
                no_select_addresses.add(e['address'])
        print(f"Resuming — loaded {len(entries)} existing entr{'y' if len(entries)==1 else 'ies'} from {out_path}.")

    # Connect
    print(f"Connecting to {port} at {BAUD} baud...")
    try:
        ser = serial.Serial(port, BAUD, timeout=0.1)
    except serial.SerialException as exc:
        sys.exit(f"Could not open port: {exc}")

    time.sleep(2)           # wait for Arduino reset triggered by DTR
    ser.reset_input_buffer()

    # Activate recv mode
    send(ser, 'recv')
    if not drain_until(ser, 'listening'):
        # Was already active — toggled off; send again
        send(ser, 'recv')
        if not drain_until(ser, 'listening'):
            print("Warning: could not confirm recv mode. Continuing anyway.")

    print("IR receive mode active.")
    print(f"Press buttons on the remote. Ctrl-C when done (saves to {out_path}).\n")

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
            key      = (address, command)

            if key in seen:
                entry = seen[key]
                if entry is not None:
                    sel   = entry.get('select') or entry.get('player')
                    label = f" / {sel}" if sel is not None else ''
                    print(f"  ✓  {entry['name']}{label}")
                continue

            print(f"  Captured  Protocol={protocol}  Address={address}  Command={command}  {bits} bits")

            # Name — pre-fill if this command code was already named on another position
            if command in command_names:
                suggested = command_names[command]
                raw = input(f"  Button name [{suggested}]: ").strip()
                name = raw if raw else suggested
            else:
                name = input("  Button name (Enter to skip): ").strip()
                if not name:
                    seen[key] = None
                    print("  Skipped.\n")
                    continue

            command_names[command] = name

            # Select switch — only ask if --selects was given
            select = None
            if selects:
                if address in no_select_addresses:
                    pass  # this address never uses the select switch — skip silently
                else:
                    suggested_sel = address_selects.get(address)
                    select = prompt_select(selects, suggested=suggested_sel)
                    if select is None:
                        no_select_addresses.add(address)

            entry = {
                "name":     name,
                "protocol": protocol,
                "address":  address,
                "command":  command,
                "bits":     bits,
            }
            if selects:
                entry["select"] = select

            entries.append(entry)
            seen[key] = entry
            if select is not None:
                address_selects[address] = select

            label = f" / {select}" if select is not None else ''
            print(f"  Saved: {name}{label}\n")

    except KeyboardInterrupt:
        print("\nStopping...")

    finally:
        try:
            send(ser, 'recv')  # toggle recv off
            time.sleep(0.2)
            ser.close()
        except Exception:
            pass

        real = [e for e in entries if e]
        if real:
            with out_path.open('w') as f:
                json.dump(real, f, indent=2)
            print(f"Saved {len(real)} entr{'y' if len(real)==1 else 'ies'} to {out_path}.")
        else:
            print("Nothing to save.")


if __name__ == '__main__':
    main()
