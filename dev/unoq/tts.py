#!/usr/bin/env python3
"""tts.py — speak text on the Arduino Uno Q with Piper (nicer voice than espeak), over adb.

  python tts.py "the robot is ready"
  python tts.py hello there              # quotes optional — args are joined
  python tts.py --voice en_US-amy-low "faster voice"
  python tts.py --wake "cold start"      # wake-burst first (un-clip the first word)
  python tts.py --oneshot "force fresh"  # skip the daemon even if it's running

Speaks through the default sink (your BT speaker). If the warm TTS daemon is running
(tts-daemon.service — see setup-tts.py) this just hands it the text over a FIFO (instant: the
model's already loaded). Otherwise it falls back to a one-shot piper synth, which pays the ~10 s
model load each call — fine for occasional use, but that's what the daemon exists to avoid.
(espeak.py is instant + low quality; tts.py is nicer Piper voices.)
"""
import base64
import shlex
import subprocess
import sys

UENV = 'XDG_RUNTIME_DIR=/run/user/$(id -u) DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$(id -u)/bus'
VOICE_DIR = "$HOME/.local/share/piper"
FIFO = "/run/user/$(id -u)/tts.fifo"
WAKE_WAV = "/usr/share/sounds/alsa/Front_Center.wav"


def usr(cmd, timeout=60):
    try:
        p = subprocess.run(["adb", "shell", f"{UENV} {cmd}"],
                          capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return 124, "(timed out)"
    return p.returncode, (p.stdout + p.stderr).strip()


def have_board():
    d = subprocess.run(["adb", "devices"], capture_output=True, text=True)
    return any(l.strip().endswith("device") for l in d.stdout.splitlines()[1:])


def daemon_running():
    rc, _ = usr(f"test -p {FIFO}")
    return rc == 0


def speak_daemon(text, voice):
    # Hand the line to the warm daemon over its FIFO (base64 so any text/quotes survive). The
    # daemon synthesizes + plays; this returns instantly (fire-and-forget).
    b64 = base64.b64encode(f"{voice}:{text}".encode()).decode()
    return usr(f"sh -c 'echo {b64} | base64 -d > {FIFO}'")


def speak_oneshot(text, voice, wake):
    # Fallback: a fresh piper synth (pays the ~10 s model load). base64 the text so no quoting
    # issues; PATH for piper, UENV so paplay reaches the default (BT) sink.
    b64 = base64.b64encode(text.encode()).decode()
    onnx = f"{VOICE_DIR}/{voice}.onnx"
    wake_cmd = f"paplay {WAKE_WAV} 2>/dev/null; sleep 0.4; " if wake else ""
    script = (f"export PATH=$HOME/.local/bin:$PATH; "
              f"echo {b64} | base64 -d | piper -m {onnx} -f /tmp/tts.wav && "
              f"{wake_cmd}paplay /tmp/tts.wav")
    return usr(f"sh -c {shlex.quote(script)}")


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__.strip())
        return

    voice, wake, oneshot = "en_US-amy-medium", False, False
    while args and args[0].startswith("--"):
        if args[0] == "--wake":
            wake, args = True, args[1:]
        elif args[0] == "--oneshot":
            oneshot, args = True, args[1:]
        elif args[0] == "--voice" and len(args) >= 2:
            voice, args = args[1], args[2:]
        else:
            sys.exit(f"unknown option '{args[0]}' — see --help")
    text = " ".join(args).strip()
    if not text:
        sys.exit("nothing to say — usage: tts.py \"some text\"")

    if not have_board():
        sys.exit("no adb device — plug the Uno Q in over USB")

    # Prefer the warm daemon (instant); --wake/--oneshot force the fresh-synth path.
    if not oneshot and not wake and daemon_running():
        rc, out = speak_daemon(text, voice)
    else:
        rc, out = speak_oneshot(text, voice, wake)
    if rc:
        sys.exit(f"tts failed: {out}\n"
                 "(no piper? run setup-tts.py. no sound? bt.py connect.)")


if __name__ == "__main__":
    main()
