#!/usr/bin/env python3
"""ir_speak.py — like ir_lookup.py, but SPEAKS the matched button name aloud (Uno Q).

A pure subscriber to the broker's ch1 socket — it starts nothing (the board streams IR on
ch1 as a standing capability; one-time setup `cmdr autostart add "ir recv"`). Matches each
press against the JSON maps in maps/, prints the match, and SPEAKS the button's name through Piper
TTS. It reuses your existing TTS method by driving ~/piper_project/tts_stream.py as a
persistent co-process: that script loads the voice once and speaks each line piped to its
`tts>` stdin loop, so the model stays warm between presses and ir_speak.py itself stays on the
plain system python3 like the other channel tools (the TTS deps live in the piper venv, which
only the co-process needs). If piper isn't available (or dies), it falls back to **espeak-ng**
so it still speaks. Run it on the SBC next to the broker. A test/demo tool.

Usage:
    python3 ir_speak.py [--maps DIR] [--rundir DIR] [--piper-dir DIR]

    --maps      DIR   directory of JSON map files (default: maps/)
    --rundir    DIR   broker rundir holding ch1.sock (default: /tmp/commander)
    --piper-dir DIR   piper TTS project dir (default: ~/piper_project) — holds venv/ + tts_stream.py
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from irchan import ChannelLink

DEFAULT_MAPS = "maps"

# A held/pressed button retransmits; we announce a button once and re-announce only after a
# release. Reads drain ch1 to the newest frame (real-time, no backlog), and two guards keep a
# switch clean: (1) a frame must be the latest for TWO consecutive reads to count — a lone
# stray/trailing frame of the button you just left (what made the OLD button announce on a
# switch) is superseded by the new button before it confirms, so it's dropped; a real press is
# always several frames, so it confirms. (2) a gap with no frames (RELEASE_IDLE) marks a release
# so the SAME button pressed again re-announces.
RELEASE_IDLE = 0.3   # seconds of no ch1 frames before a button counts as released

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
    """Speaks button names. Primary backend: the user's tts_stream.py driven as a warm
    co-process (reusing their script verbatim — its `tts>` input loop — keeps this in lockstep
    with whatever Piper voice/method it uses, and loads the model once). Backup: espeak-ng,
    one process per utterance (it starts fast, no warm-up). Falls back if piper is missing at
    start or dies mid-run, so a degraded run still speaks."""
    def __init__(self, piper_dir):
        self.proc = None                                    # piper co-process, when live
        self.espeak = shutil.which("espeak-ng")             # backup CLI, or None
        py = os.path.join(piper_dir, "venv", "bin", "python")
        script = os.path.join(piper_dir, "tts_stream.py")
        missing = py if not os.path.exists(py) else (script if not os.path.exists(script) else None)
        if not missing:
            try:
                self.proc = subprocess.Popen(
                    [py, script],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.DEVNULL,   # hide its "Loading voice"/`tts>` chatter; we print our own
                    text=True,
                )
            except OSError as exc:
                missing = f"tts_stream.py ({exc})"
        if self.proc:
            return                                          # piper is primary
        if self.espeak:
            print(f"[ir_speak] piper TTS unavailable ({missing} not found) — using espeak-ng "
                  f"backup.", file=sys.stderr)
        else:
            print(f"[ir_speak] TTS disabled — {missing} not found and no espeak-ng; matches "
                  f"print only.", file=sys.stderr)

    @property
    def backend(self):
        if self.proc is not None and self.proc.poll() is None:
            return "piper"
        return "espeak" if self.espeak else None

    def say(self, text):
        text = text.replace("\n", " ")
        if self.proc is not None:                           # primary: warm piper co-process
            if self.proc.poll() is None:
                try:
                    self.proc.stdin.write(text + "\n")      # one line = one utterance
                    self.proc.stdin.flush()
                    return True
                except (BrokenPipeError, ValueError):
                    self.proc = None
            else:
                self.proc = None                            # piper died -> fall through to espeak
        if self.espeak:                                     # backup: fire-and-forget espeak-ng
            try:
                subprocess.Popen([self.espeak, text],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                return True
            except OSError:
                self.espeak = None
        return False

    def close(self):
        if self.proc is not None and self.proc.poll() is None:
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
    p.add_argument('--rundir', default='/tmp/commander', help='broker rundir (ch1.sock)')
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
    if speaker.backend == "piper":
        print("Piper voice warming up — the first press may lag until it loads.\n")
    elif speaker.backend == "espeak":
        print("Speaking matched names via espeak-ng.\n")

    link = ChannelLink(args.rundir)
    print(link.hint())
    print("Press any button.  Ctrl-C to quit.\n")

    announced, armed, prev, last_seen = None, False, None, 0.0
    try:
        for line in link.latest_events():
            now = time.monotonic()
            if line is None:                       # idle tick — no frame this poll
                if announced is not None and now - last_seen > RELEASE_IDLE:
                    armed, prev = True, None        # released — let the same button re-announce
                continue
            m = IR_RE.search(line)
            if not m or 'Repeat' in line:
                continue
            address  = m.group(2).lower()
            command  = m.group(3).lower()
            last_seen = now
            key = (address, command)
            confirmed, prev = (key == prev), key   # latest twice -> drop lone stray/trailing frames
            if not confirmed:
                continue
            if key == announced and not armed:     # same button still held — already announced
                continue
            announced, armed = key, False          # new (or re-pressed) button — announce it
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
