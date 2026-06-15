#!/usr/bin/env python3
"""ir_speak.py — like ir_lookup.py, but SPEAKS the matched button name aloud (Uno Q).

Reads IR presses from the broker's ch1 socket (driving `ir recv` over ch0), matches each
against the JSON maps in maps/, prints the match, and SPEAKS the button's name through Piper
TTS. It reuses your existing TTS method by driving ~/piper_project/tts_stream.py as a
persistent co-process: that script loads the voice once and speaks each line piped to its
`tts>` stdin loop, so the model stays warm between presses and ir_speak.py itself stays on the
plain system python3 like the other channel tools (the TTS deps live in the piper venv, which
only the co-process needs). Run it on the SBC next to the broker. A test/demo tool.

Usage:
    python3 ir_speak.py [--maps DIR] [--rundir DIR] [--piper-dir DIR]

    --maps      DIR   directory of JSON map files (default: maps/)
    --rundir    DIR   broker rundir holding ch0.sock/ch1.sock (default: /tmp/commander)
    --piper-dir DIR   piper TTS project dir (default: ~/piper_project) — holds venv/ + tts_stream.py
"""
import argparse
import json
import os
import re
import subprocess
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


class Speaker:
    """Drives tts_stream.py as a warm co-process. Reusing your script verbatim (its `tts>`
    input loop) instead of re-implementing the TTS keeps this in lockstep with whatever
    voice/method that script uses, and loads the model once."""
    def __init__(self, piper_dir):
        self.proc = None
        py = os.path.join(piper_dir, "venv", "bin", "python")
        script = os.path.join(piper_dir, "tts_stream.py")
        missing = py if not os.path.exists(py) else (script if not os.path.exists(script) else None)
        if missing:
            print(f"[ir_speak] TTS disabled — {missing} not found; matches will print but not "
                  f"speak. (point --piper-dir at your piper project)", file=sys.stderr)
            return
        try:
            self.proc = subprocess.Popen(
                [py, script],
                stdin=subprocess.PIPE,
                stdout=subprocess.DEVNULL,   # hide its "Loading voice"/`tts>` chatter; we print our own
                text=True,
            )
        except OSError as exc:
            print(f"[ir_speak] TTS disabled — could not start tts_stream.py: {exc}", file=sys.stderr)

    @property
    def live(self):
        return self.proc is not None and self.proc.poll() is None

    def say(self, text):
        if not self.live:
            return False
        try:
            self.proc.stdin.write(text.replace("\n", " ") + "\n")   # one line = one utterance
            self.proc.stdin.flush()
            return True
        except (BrokenPipeError, ValueError):
            self.proc = None
            return False

    def close(self):
        if self.live:
            try:
                self.proc.stdin.close()      # EOF -> tts_stream prints "bye" and exits cleanly
            except Exception:
                pass
            try:
                self.proc.wait(timeout=5)
            except Exception:
                self.proc.terminate()


def show(protocol, address, command, bits, matches, spoke):
    raw = f"{protocol:<8}  {address:<8}  {command:<6}  {bits}b"
    if not matches:
        print(f"  ?  {raw}")
        return
    tag = "🔊" if spoke else "  "
    if len(matches) == 1:
        print(f"  ✓ {tag}  {raw}  →  {fmt_match(*matches[0])}")
    else:
        print(f"  ✓ {tag}  {raw}  →  {len(matches)} matches")
        for remote, entry in matches:
            print(f"          {fmt_match(remote, entry)}")


def main():
    try:
        sys.stdout.reconfigure(line_buffering=True)   # stream even when piped (adb shell 'cmd')
    except Exception:
        pass
    p = argparse.ArgumentParser(description="Speak the matched IR button name over the channel bus")
    p.add_argument('--maps', '-m', default=DEFAULT_MAPS, metavar='DIR', help='map files directory')
    p.add_argument('--rundir', default='/tmp/commander', help='broker rundir (ch0.sock/ch1.sock)')
    p.add_argument('--piper-dir', default=os.path.expanduser('~/piper_project'),
                   help='piper TTS project dir (holds venv/ + tts_stream.py)')
    args = p.parse_args()

    maps = load_maps(args.maps)
    if maps:
        total = sum(len(v) for v in maps.values())
        summary = '  '.join(f"{name} ({len(e)})" for name, e in maps.items())
        print(f"Loaded {len(maps)} map{'s' if len(maps) != 1 else ''}  [{total} entries]\n  {summary}\n")
    else:
        print(f"No map files in '{args.maps}/' — nothing to match/speak. Build one with ir_map.py.\n")

    speaker = Speaker(args.piper_dir)
    if speaker.live:
        print("Piper voice warming up — the first press may lag until it loads.\n")

    link = ChannelLink(args.rundir)
    link.enable_recv()
    print("Listening — press any button.  Ctrl-C to quit.\n")

    try:
        for line in link.events_lines():
            m = IR_RE.search(line)
            if not m or 'Repeat' in line:
                continue
            address  = m.group(2).lower()
            command  = m.group(3).lower()
            matches  = lookup(maps, address, command)
            spoke = speaker.say(matches[0][1]['name']) if matches else False
            show(m.group(1), address, command, int(m.group(5)), matches, spoke)
    except KeyboardInterrupt:
        print("\nDone.")
    finally:
        link.close()
        speaker.close()


if __name__ == '__main__':
    main()
