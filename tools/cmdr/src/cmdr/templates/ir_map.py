#!/usr/bin/env python3
"""ir_map.py — build a named IR button map from the commander channel bus (Uno Q).

The channel-bus counterpart of irmap.py: instead of a serial console it reads presses from
the broker's ch1 socket (a pure subscriber, see irchan.py). It starts nothing — the board
streams IR on ch1 as a standing capability (one-time: `cmdr autostart add "ir recv"`), so
the human console stays private. Run it on the SBC next to the broker. The output JSON is
identical to irmap.py's, so the maps are interchangeable and irlookup/ir_lookup read them
the same way.

Usage:
    python3 ir_map.py [--output FILE] [--selects NAME ...] [--rundir DIR]

    --output FILE     output JSON map (default: ir_map.json)
    --selects NAME…   names for each select-switch position, left to right (omit if none)
    --rundir  DIR     broker rundir holding ch1.sock (default: /tmp/commander)
"""
import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from irchan import ChannelLink

DEFAULT_OUT = "ir_map.json"

IR_RE = re.compile(
    r'Protocol=(\w+)\s+Address=(0x[0-9A-Fa-f]+),\s+'
    r'Command=(0x[0-9A-Fa-f]+),\s+Raw-Data=(0x[0-9A-Fa-f]+),\s+'
    r'(\d+)\s+bits'
)


def prompt_select(selects, suggested=None):
    """Ask which select-switch position this button was on. Name, 1-based index, or Enter."""
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
    if raw.isdigit():
        idx = int(raw) - 1
        if 0 <= idx < len(selects):
            return selects[idx]
    for name in selects:
        if raw.lower() == name.lower():
            return name
    return None


def main():
    try:
        sys.stdout.reconfigure(line_buffering=True)   # stream even when piped (adb shell 'cmd')
    except Exception:
        pass
    p = argparse.ArgumentParser(description="Map IR remote buttons over the commander channel bus")
    p.add_argument('--output', '-o', default=DEFAULT_OUT, help='output JSON file')
    p.add_argument('--selects', nargs='*', metavar='NAME', help='select-switch position names, left to right')
    p.add_argument('--rundir', default='/tmp/commander', help='broker rundir (ch1.sock)')
    args = p.parse_args()

    out_path = Path(args.output)
    selects = args.selects or []

    if selects:
        print(f"Select switch positions: {', '.join(f'{i+1}={n}' for i, n in enumerate(selects))}")

    # Resume from an existing map.
    entries, seen, command_names, address_selects, no_select_addresses = [], {}, {}, {}, set()
    if out_path.exists():
        entries = json.loads(out_path.read_text())
        for e in entries:
            seen[(e['address'], e['command'])] = e
            if e.get('name'):
                command_names[e['command']] = e['name']
            sel = e.get('select') or e.get('player')
            if sel is not None:
                address_selects[e['address']] = str(sel)
            elif 'select' in e and e['select'] is None:
                no_select_addresses.add(e['address'])
        print(f"Resuming — loaded {len(entries)} existing entr{'y' if len(entries)==1 else 'ies'} from {out_path}.")

    link = ChannelLink(args.rundir)
    print(link.hint())
    print(f"Press buttons on the remote. Ctrl-C when done (saves to {out_path}).\n")

    try:
        for line in link.events_lines():
            m = IR_RE.search(line)
            if not m or 'Repeat' in line:
                continue
            protocol = m.group(1)
            address  = m.group(2).lower()
            command  = m.group(3).lower()
            bits     = int(m.group(5))
            key      = (address, command)

            if key in seen:
                entry = seen[key]
                if entry is not None:
                    sel = entry.get('select') or entry.get('player')
                    label = f" / {sel}" if sel is not None else ''
                    print(f"  ✓  {entry['name']}{label}")
                continue

            print(f"  Captured  Protocol={protocol}  Address={address}  Command={command}  {bits} bits")

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

            select = None
            if selects and address not in no_select_addresses:
                select = prompt_select(selects, suggested=address_selects.get(address))
                if select is None:
                    no_select_addresses.add(address)

            entry = {"name": name, "protocol": protocol, "address": address,
                     "command": command, "bits": bits}
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
        link.close()
        real = [e for e in entries if e]
        if real:
            out_path.write_text(json.dumps(real, indent=2))
            print(f"Saved {len(real)} entr{'y' if len(real)==1 else 'ies'} to {out_path}.")
        else:
            print("Nothing to save.")


if __name__ == '__main__':
    main()
