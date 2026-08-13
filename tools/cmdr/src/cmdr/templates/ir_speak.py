#!/usr/bin/env python3
"""ir_speak.py — like ir_lookup.py, but SPEAKS the matched button name aloud (Uno Q).

A pure subscriber to the broker's ch1 socket — it starts nothing (the board streams IR on
ch1 as a standing capability; one-time setup `cmdr autostart add "ir recv"`). Matches each
press against the JSON maps in maps/, prints the match, and SPEAKS the button's name.

TTS backends, best-first: (1) the Piper **TTS daemon** from the unoq-tools repo
(https://github.com/gbryant/unoq-tools — `setup-tts.py`, then `tts.py daemon install`), which
keeps the voice warm and takes fire-and-forget lines on a FIFO (`/run/user/<uid>/tts.fifo`;
its existence is the readiness signal); (2) **espeak-ng**, one fast-starting process per
utterance; (3) print-only. The fallback is per-utterance, so a daemon restart mid-run just
means a few robotic announcements. ir_speak.py itself stays on the plain system python3 like
the other channel tools (the Piper deps live in the daemon's venv, on the board).
Run it on the SBC next to the broker. A test/demo tool.

Usage:
    python3 ir_speak.py [--maps DIR] [--rundir DIR] [--tts-fifo PATH] [--greeting [TEXT]]

    --maps      DIR    directory of JSON map files (default: maps/)
    --rundir    DIR    broker rundir holding ch1.sock (default: /tmp/commander)
    --tts-fifo  PATH   TTS daemon FIFO (default: $XDG_RUNTIME_DIR/tts.fifo,
                       else /run/user/<uid>/tts.fifo)
    --greeting  TEXT   speak TEXT once at startup, after the channel link is up ("ready" if
                       the flag is given bare). For a board that boots into this with no
                       screen attached, it is the only sign the chain came up.
"""
import argparse
import json
import os
import re
import shutil
import stat
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


# Some remotes flip bit 7 of the command between otherwise identical frames — a toggle that
# distinguishes a new press from a held one. A Hisense Roku sends power as 0x17 then 0x97, down
# as 0x33 then 0xb3. Maps record the base form (that's what irmap.py captured), so both the
# identity of a press and the lookup have to see through the toggle, or half the frames are
# strangers and no two consecutive frames ever agree.
TOGGLE_BIT = 0x80


def base_command(command):
    """The command with the toggle bit cleared — the form the maps store."""
    return hex(int(command, 16) & ~TOGGLE_BIT)


def press_id(key):
    """Identity of a press: same button whether or not this frame carried the toggle bit."""
    address, command = key
    return (address, base_command(command))


def lookup(maps, address, command):
    # Exact first, so a remote that genuinely uses both 0x17 and 0x97 as separate buttons still
    # resolves each to its own name.
    hits = [(remote, e) for remote, entries in maps.items()
            for e in entries if e['address'] == address and e['command'] == command]
    if hits:
        return hits
    # Otherwise compare with the toggle bit cleared on BOTH sides. The shipped maps store the
    # base form, but ir_map.py records whichever frame it captured — so a map you build from a
    # toggle remote may hold 0x97 where the press arrives as 0x17, or the reverse.
    base = base_command(command)
    return [(remote, e) for remote, entries in maps.items()
            for e in entries if e['address'] == address and base_command(e['command']) == base]


def fmt_match(remote, entry):
    sel = entry.get('select') or entry.get('player')
    s = f"{remote} : {entry['name']}"
    return s + (f" / {sel}" if sel is not None else '')


def default_tts_fifo():
    """The unoq-tools TTS daemon's FIFO. The daemon (a systemd --user unit) resolves
    $XDG_RUNTIME_DIR itself; a client in a bare adb shell may not have it set, so fall
    back to the conventional /run/user/<uid> location."""
    rundir = os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
    return os.path.join(rundir, "tts.fifo")


class Speaker:
    """Speaks button names. Primary backend: the unoq-tools Piper TTS daemon — a warm
    voice behind a FIFO; one written line = one fire-and-forget utterance, and the FIFO
    only exists while the daemon is serving. Backup: espeak-ng, one process per utterance
    (it starts fast, no warm-up). The FIFO is retried per utterance, so a daemon
    restarting mid-run degrades and recovers on its own."""
    def __init__(self, fifo):
        self.fifo = fifo
        self.espeak = shutil.which("espeak-ng")             # backup CLI, or None
        if self.backend == "piper":
            return
        if self.espeak:
            print(f"[ir_speak] TTS daemon not running ({fifo} absent — see unoq-tools "
                  f"setup-tts.py) — using espeak-ng backup.", file=sys.stderr)
        else:
            print(f"[ir_speak] TTS disabled — no daemon FIFO at {fifo} and no espeak-ng; "
                  f"matches print only.", file=sys.stderr)

    @property
    def backend(self):
        try:
            if stat.S_ISFIFO(os.stat(self.fifo).st_mode):
                return "piper"
        except OSError:
            pass
        return "espeak" if self.espeak else None

    def say(self, text):
        text = text.replace("\n", " ")
        try:
            # O_NONBLOCK: fail with ENXIO instead of blocking forever if the daemon
            # isn't at the read end (e.g. it exited without cleaning up its FIFO).
            fd = os.open(self.fifo, os.O_WRONLY | os.O_NONBLOCK)
            try:
                os.write(fd, (text + "\n").encode())
                return True
            finally:
                os.close(fd)
        except OSError:
            pass                                            # daemon down -> espeak
        if self.espeak:                                     # backup: fire-and-forget espeak-ng
            try:
                subprocess.Popen([self.espeak, text],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                return True
            except OSError:
                self.espeak = None
        return False

    def close(self):
        pass                                                # nothing held open between utterances


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
    p.add_argument('--tts-fifo', default=default_tts_fifo(),
                   help='TTS daemon FIFO (unoq-tools tts_daemon.py)')
    # For a headless board that boots into this (see ./deploy-sbc --service): with no screen,
    # a spoken line is the only way to learn that the speaker connected, the voice is warm and
    # the broker socket exists. Off unless asked for, so an interactive run stays quiet.
    p.add_argument('--greeting', nargs='?', const='ready', metavar='TEXT',
                   help='speak TEXT once at startup ("ready" if given with no value)')
    # Every way this tool ignores a frame is silent by design, which is exactly what makes a
    # non-speaking remote hard to diagnose: you can't tell "the board sent nothing" from "I threw
    # it away". --debug narrates the discards.
    p.add_argument('--debug', action='store_true',
                   help='log every frame that is dropped, and why')
    args = p.parse_args()

    maps = load_maps(args.maps)
    if maps:
        total = sum(len(v) for v in maps.values())
        summary = '  '.join(f"{name} ({len(e)})" for name, e in maps.items())
        print(f"Loaded {len(maps)} map{'s' if len(maps) != 1 else ''}  [{total} entries]\n  {summary}\n")
    else:
        print(f"No map files in '{args.maps}/' — nothing to match/speak. Build one with ir_map.py.\n")

    speaker = Speaker(args.tts_fifo)
    if speaker.backend == "piper":
        print("Speaking matched names via the Piper TTS daemon (voice already warm).\n")
    elif speaker.backend == "espeak":
        print("Speaking matched names via espeak-ng.\n")

    link = ChannelLink(args.rundir)
    print(link.hint())
    print("Press any button.  Ctrl-C to quit.\n")

    # Greet AFTER the link is built, so hearing it means the whole chain is up, not just TTS.
    if args.greeting:
        print(f"[ir_speak] greeting: {args.greeting}")
        speaker.say(args.greeting)

    # One announcement per press, debounced by TIME rather than by repetition. Announce the
    # first frame, then ignore every frame until they stop for RELEASE_IDLE — which covers a
    # held button's repeats, a toggle remote's partner frame, and a stray trailing frame from
    # the remote you just switched away from, all with one rule.
    #
    # The previous rule waited for two consecutive frames that agreed, to drop those strays. It
    # silently assumed every remote repeats: a Vizio sound bar sends each press exactly ONCE, so
    # nothing was ever confirmed and it announced nothing at all — no speech, and not even a "?".
    def dropped(reason, detail):
        if args.debug:
            print(f"  · dropped [{reason}] {detail}", flush=True)

    announced, last_seen = None, 0.0
    try:
        for line in link.latest_events():
            now = time.monotonic()
            if line is None:                       # idle tick — no frame this poll
                if announced is not None and now - last_seen > RELEASE_IDLE:
                    announced = None                # released — arm for the next press
                continue
            m = IR_RE.search(line)
            if not m:
                dropped("unparsed", line.strip())   # not an IR event line, or a format change
                continue
            if 'Repeat' in line:
                dropped("repeat frame", line.strip())
                continue
            address  = m.group(2).lower()
            command  = m.group(3).lower()
            gap = now - last_seen
            last_seen = now
            key = (address, command)
            if announced is not None:              # still inside the current press — ignore
                dropped("same press", f"{address}/{command}  {gap * 1000:.0f} ms after the last "
                                      f"frame (announced {announced[0]}/{announced[1]})")
                continue
            announced = press_id(key)
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
