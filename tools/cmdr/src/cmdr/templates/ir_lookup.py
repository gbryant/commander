#!/usr/bin/env python3
"""ir_lookup.py — identify live IR presses against saved maps, over the channel bus (Uno Q).

The channel-bus counterpart of irlookup.py: a pure subscriber to the broker's ch1 socket —
matches each press against every JSON map in maps/. It starts nothing: the board streams IR
on ch1 as a standing capability (one-time setup: `cmdr autostart add "ir recv"`), so this
tool just listens and never touches the human console. Run it on the SBC next to the broker.
Map files are the same format ir_map.py / irmap.py produce.

Usage:
    python3 ir_lookup.py [--maps DIR] [--rundir DIR]

    --maps   DIR   directory of JSON map files (default: maps/)
    --rundir DIR   broker rundir holding ch1.sock (default: /tmp/commander)
"""
import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from irchan import ChannelLink

DEFAULT_MAPS = "maps"

IR_RE = re.compile(
    r'Protocol=(\w+)\s+Address=(0x[0-9A-Fa-f]+),\s+'
    r'Command=(0x[0-9A-Fa-f]+),\s+Raw-Data=(0x[0-9A-Fa-f]+),\s+'
    r'(\d+)\s+bits'
)


def load_maps(maps_dir):
    maps = {}
    path = Path(maps_dir)
    if not path.exists():
        return maps
    for f in sorted(path.glob('*.json')):
        try:
            maps[f.stem] = json.loads(f.read_text())
        except Exception as exc:
            print(f"Warning: skipping {f.name}: {exc}", file=sys.stderr)
    return maps


def lookup(maps, address, command):
    return [(remote, e) for remote, entries in maps.items()
            for e in entries if e['address'] == address and e['command'] == command]


def fmt_match(remote, entry):
    sel = entry.get('select') or entry.get('player')
    s = f"{remote} : {entry['name']}"
    return s + (f" / {sel}" if sel is not None else '')


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


def main():
    try:
        sys.stdout.reconfigure(line_buffering=True)   # stream even when piped (adb shell 'cmd')
    except Exception:
        pass
    p = argparse.ArgumentParser(description="Identify IR presses against saved maps over the channel bus")
    p.add_argument('--maps', '-m', default=DEFAULT_MAPS, metavar='DIR', help='map files directory')
    p.add_argument('--rundir', default='/tmp/commander', help='broker rundir (ch1.sock)')
    args = p.parse_args()

    maps = load_maps(args.maps)
    if maps:
        total = sum(len(v) for v in maps.values())
        summary = '  '.join(f"{name} ({len(e)})" for name, e in maps.items())
        print(f"Loaded {len(maps)} map{'s' if len(maps) != 1 else ''}  [{total} entries]\n  {summary}\n")
    else:
        print(f"No map files in '{args.maps}/' — all presses report as unknown. Build one with ir_map.py.\n")

    link = ChannelLink(args.rundir)
    print(link.hint())
    print("Press any button.  Ctrl-C to quit.\n")

    try:
        for line in link.events_lines():
            m = IR_RE.search(line)
            if not m or 'Repeat' in line:
                continue
            protocol = m.group(1)
            address  = m.group(2).lower()
            command  = m.group(3).lower()
            bits     = int(m.group(5))
            show(protocol, address, command, bits, lookup(maps, address, command))
    except KeyboardInterrupt:
        print("\nDone.")
    finally:
        link.close()


if __name__ == '__main__':
    main()
